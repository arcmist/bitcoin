/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "node.hpp"

#ifdef PROFILER_ON
#include "profiler.hpp"
#include "profiler_setup.hpp"
#endif

#include "digest.hpp"
#include "info.hpp"
#include "message.hpp"
#include "block.hpp"
#include "chain.hpp"
#include "interpreter.hpp"
#include "daemon.hpp"


#define PEER_MESSAGE_LIMIT 5000
#define PEER_TIME_LIMIT 1800


namespace BitCoin
{
    unsigned int Node::mNextID = 256;

    Node::Node(NextCash::Network::Connection *pConnection, uint32_t pConnectionType,
      uint64_t pServices, Daemon *pDaemon, bool *pStopFlag, bool pAnnounceCompact) :
      mConnectionMutex("Node Connection"), mStatisticsLock("Statistics"),
      mMessagesToSendLock("Node Messages"), mFilter(BloomFilter::STANDARD),
      mBlockRequestMutex("Node Block Request"), mAnnounceMutex("Node Announce"),
      mAnnounceBlockMutex("Node Announce Block")
    {
        mConnectionType = pConnectionType;
        mChainID = CHAIN_UNKNOWN;
        mRequestAnnounceCompact = pAnnounceCompact;
        mConnection = pConnection;
        mAddress = pConnection->ip();
        mServices = pServices;
        mDaemon = pDaemon;
        mStopFlag = pStopFlag;
        mChain = pDaemon->chain();
        if(isOutgoing())
            mMonitor = pDaemon->monitor();
        else
            mMonitor = NULL;
        mConnected = false;
        mIsInitialized = false;
        mPrepared = false;
        mVersionSent = false;
        mVersionAcknowledged = false;
        mVersionAcknowledgeSent = false;
        mSendHeaders = false;
        mMinimumFeeRate = 0;
        mSentVersionData = NULL;
        mReceivedVersionData = NULL;
        mHeaderRequestTime = 0;
        mBlockRequestTime = 0;
        mLastBlockReceiveTime = 0;
        mLastReceiveTime = getTime();
        mLastCheckTime = getTime();
        mLastBlackListCheck = getTime();
        mLastSavedCheckTime = 0;
        mLastPingNonce = 0;
        mLastPingTime = 0;
        mPingRoundTripTime = 0xffffffffffffffff;
        mPingCutoff = 10;
        mBlockDownloadCount = 0;
        mBlockDownloadSize = 0;
        mBlockDownloadTime = 0;
        mMessagesReceived = 0;
        mOldTransactionCount = 0;
        mPingCount = 0;
        mConnectedTime = getTime();
        mLastExpireTime = mConnectedTime;
        mStarted = false;
        mStopRequested = false;
        mStopped = false;
        mAnnounceBlocksCompact = false;
        mSendCompactBlocksVersion = 0L;
        mSendCompactSent = false;
        mRejected = false;
        mWasReady = false;
        mReleased = false;
        mMemPoolRequested = false;
        mProcessingCompactTransactions = false;
        mActiveMerkleRequests = 0;
        mLastMerkleCheck = 0;
        mLastMerkleRequest = 0;
        mLastMerkleReceive = 0;
        mBloomFilterID = 0;
        mInventoryData = NULL;
        mPeersRequested = false;

        mID = mNextID++;
        if(isSeed())
            mName.writeFormatted("Node seed[%d]", mID);
        else if(isScan())
            mName.writeFormatted("Node scan[%d]", mID);
        else if(isIncoming())
            mName.writeFormatted("Node i[%d]", mID);
        else
            mName.writeFormatted("Node o[%d]", mID);

#ifndef SINGLE_THREAD
        if(isScan() && mStopFlag != NULL)
        {
            mThread = NULL;
            runInThread();
        }
        else
            mThread = new NextCash::Thread("Node", run, this);
#endif
    }

    Node::Node(NextCash::Network::IPAddress &pIPAddress, uint32_t pConnectionType,
      uint64_t pServices, Daemon *pDaemon, bool pAnnounceCompact) :
      mConnectionMutex("Node Connection"), mStatisticsLock("Statistics"),
      mMessagesToSendLock("Node Messages"), mFilter(BloomFilter::STANDARD),
      mBlockRequestMutex("Node Block Request"), mAnnounceMutex("Node Announce"),
      mAnnounceBlockMutex("Node Announce Block")
    {
        mConnectionType = pConnectionType;
        mChainID = CHAIN_UNKNOWN;
        mRequestAnnounceCompact = pAnnounceCompact;
        mAddress = pIPAddress;
        mConnection = NULL;
        mServices = pServices;
        mDaemon = pDaemon;
        mStopFlag = NULL;
        mChain = pDaemon->chain();
        if(isOutgoing())
            mMonitor = pDaemon->monitor();
        else
            mMonitor = NULL;
        mConnected = false;
        mIsInitialized = false;
        mPrepared = false;
        mVersionSent = false;
        mVersionAcknowledged = false;
        mVersionAcknowledgeSent = false;
        mSendHeaders = false;
        mMinimumFeeRate = 0;
        mSentVersionData = NULL;
        mReceivedVersionData = NULL;
        mHeaderRequestTime = 0;
        mBlockRequestTime = 0;
        mLastBlockReceiveTime = 0;
        mLastReceiveTime = getTime();
        mLastCheckTime = getTime();
        mLastBlackListCheck = getTime();
        mLastSavedCheckTime = 0;
        mLastPingNonce = 0;
        mLastPingTime = 0;
        mPingRoundTripTime = 0xffffffffffffffff;
        mPingCutoff = 10;
        mBlockDownloadCount = 0;
        mBlockDownloadSize = 0;
        mBlockDownloadTime = 0;
        mMessagesReceived = 0;
        mOldTransactionCount = 0;
        mPingCount = 0;
        mConnectedTime = getTime();
        mLastExpireTime = mConnectedTime;
        mStarted = false;
        mStopRequested = false;
        mStopped = false;
        mAnnounceBlocksCompact = false;
        mSendCompactBlocksVersion = 0L;
        mSendCompactSent = false;
        mRejected = false;
        mWasReady = false;
        mReleased = false;
        mMemPoolRequested = false;
        mProcessingCompactTransactions = false;
        mActiveMerkleRequests = 0;
        mLastMerkleCheck = 0;
        mLastMerkleRequest = 0;
        mLastMerkleReceive = 0;
        mBloomFilterID = 0;
        mInventoryData = NULL;
        mPeersRequested = false;

        mID = mNextID++;
        if(isSeed())
            mName.writeFormatted("Node seed[%d]", mID);
        else if(isScan())
            mName.writeFormatted("Node scan[%d]", mID);
        else if(isIncoming())
            mName.writeFormatted("Node i[%d]", mID);
        else
            mName.writeFormatted("Node o[%d]", mID);

#ifndef SINGLE_THREAD
        // Start thread
        mThread = new NextCash::Thread(mName, run, this);
        NextCash::Thread::sleep(200); // Give the thread a chance to initialize
#endif
    }

    Node::~Node()
    {
        // Wait for thread initialize
        int timeout = 25;
        while(!mStarted && --timeout)
            NextCash::Thread::sleep(200);

        if(mConnected)
            NextCash::Log::add(NextCash::Log::VERBOSE, mName, "Disconnecting");

        requestStop();
#ifndef SINGLE_THREAD
        if(mThread != NULL)
            delete mThread;
#endif

        if(!mMessageInterpreter.pendingBlockHash.isEmpty())
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Dropped block in progress %d KB (%d secs) : %s", mReceiveBuffer.length() / 1000,
              mMessageInterpreter.pendingBlockUpdateTime - mMessageInterpreter.pendingBlockStartTime,
              mMessageInterpreter.pendingBlockHash.hex().text());

        mConnectionMutex.lock();
        if(mConnection != NULL)
            delete mConnection;
        mConnectionMutex.unlock();
        if(mSentVersionData != NULL)
            delete mSentVersionData;
        if(mReceivedVersionData != NULL)
            delete mReceivedVersionData;

        mAnnounceBlockMutex.lock();

        for(std::vector<Message::CompactBlockData *>::iterator block =
          mIncomingCompactBlocks.begin(); block != mIncomingCompactBlocks.end(); ++block)
            delete *block;

        for(std::vector<Message::CompactBlockData *>::iterator block =
          mOutgoingCompactBlocks.begin(); block != mOutgoingCompactBlocks.end(); ++block)
            delete *block;

        mMessagesToSendLock.lock();
        for(std::vector<Message::Data *>::iterator message = mMessagesToSend.begin();
          message != mMessagesToSend.end(); ++message)
            delete *message;
        mMessagesToSendLock.unlock();

        if(mInventoryData != NULL)
            delete mInventoryData;
    }

    bool Node::isOpen()
    {
        return mConnected;
    }

    double Node::blockDownloadBytesPerSecond() const
    {
        if(mBlockDownloadSize == 0 || mBlockDownloadTime == 0)
            return 0.0;
        return (double)mBlockDownloadSize / (double)mBlockDownloadTime;
    }

    void Node::release()
    {
        if(mReleased)
            return;

        NextCash::Log::add(NextCash::Log::DEBUG, mName, "Releasing");
        if(mMonitor != NULL)
            mMonitor->release(mID);
        mBlockRequestMutex.lock();
        mChain->releaseBlocksForNode(mID);
        mChain->memPool().release(mID);
        mBlocksRequested.clear();
        mHeaderRequested.clear();
        mBlockRequestMutex.unlock();
        for(std::vector<Message::CompactBlockData *>::iterator compact =
          mOutgoingCompactBlocks.begin(); compact != mOutgoingCompactBlocks.end();)
        {
            delete *compact;
            compact = mOutgoingCompactBlocks.erase(compact);
        }
        for(std::vector<Message::CompactBlockData *>::iterator compact =
          mIncomingCompactBlocks.begin(); compact != mIncomingCompactBlocks.end();)
        {
            delete *compact;
            compact = mIncomingCompactBlocks.erase(compact);
        }
        mReleased = true;
    }

    void Node::close()
    {
        if(mConnected)
        {
            mConnectionMutex.lock();
            if(mConnection != NULL)
                mConnection->close();
            mConnectionMutex.unlock();
            mConnected = false;
        }
        requestStop();
    }

    void Node::requestStop()
    {
        mStopRequested = true;
    }

    void Node::collectStatistics(Statistics &pCollection)
    {
        mStatisticsLock.lock();
        pCollection += mStatistics;
        mStatistics.clear();
        mStatisticsLock.unlock();
    }

    void Node::prepare()
    {
        if(mLastPingTime == 0)
            sendPing();

        if(!mPrepared && isReady())
        {
            Info &info = Info::instance();
            if(!info.spvMode && isOutgoing() && mReceivedVersionData->version >= 70014 &&
              !mSendCompactSent)
            {
                // Send compact block configuration.
                Message::SendCompactData sendCompactBlocks(mRequestAnnounceCompact, 1L);
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Sending compact version 0x%08x%08x", sendCompactBlocks.version >> 32,
                  sendCompactBlocks.version & 0xffffffff);
                sendMessage(&sendCompactBlocks);
                mSendCompactSent = true;
            }

            if(isScan())
            {
                info.addPeerSuccess(mAddress, 1);
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Peer scanned at %s", mAddress.text().text());
                requestHeaders();
            }
            else if(isSeed())
                requestHeaders(); // Determine chain before requesting peers
            else if(isOutgoing())
            {
                if(info.spvMode)
                    sendBloomFilter();
                else
                    sendFeeFilter();

                Message::Data sendHeadersMessage(Message::SEND_HEADERS);
                sendMessage(&sendHeadersMessage);

                requestHeaders();

                info.addPeerSuccess(mAddress, 1);
            }

            mPrepared = true;
        }
    }

    bool Node::check()
    {
        Time time = getTime();
        mLastCheckTime = time;

        if(!isOpen())
            return false;

        mConnectionMutex.lock();
        mStatisticsLock.lock();
        mStatistics.bytesReceived += mConnection->bytesReceived();
        mStatistics.bytesSent += mConnection->bytesSent();
        mConnection->resetByteCounts();
        mStatisticsLock.unlock();
        mConnectionMutex.unlock();

        Info &info = Info::instance();

        if(isOutgoing() && !info.spvMode && mSentVersionData != NULL &&
          !mSentVersionData->relay && info.initialBlockDownloadIsComplete() && mChain->isInSync())
        {
            NextCash::Log::add(NextCash::Log::INFO, mName,
              "Dropping. To add relaying node.");
            close();
            return false;
        }

        if(isSeed() && time - mConnectedTime > 60)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName,
              "Dropping. Seed connected for too long.");
            close();
            return false;
        }

        if(mPingRoundTripTime == 0xffffffffffffffff && (time - mConnectedTime) > mPingCutoff)
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Dropping. Not ready within %d seconds of connection.", mPingCutoff);
            info.addPeerFail(mAddress, 5);
            close();
            return false;
        }

        uint32_t blockTimeout = 15;
        if(mChain->isInSync())
            blockTimeout = 5;
        if(!mProcessingCompactTransactions && mBlocksRequested.size() > 0 &&
          time - mBlockRequestTime > blockTimeout && time - mLastBlockReceiveTime > blockTimeout)
        {
            // Haven't received more of the block in the last 15 seconds
            if(mMessageInterpreter.pendingBlockUpdateTime == 0 ||
              time - mMessageInterpreter.pendingBlockUpdateTime > blockTimeout)
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. No update on block for %d seconds", blockTimeout);
                info.addPeerFail(mAddress);
                close();
                return false;
            }
        }

        if(!mHeaderRequested.isEmpty() && time - mHeaderRequestTime > 15)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Not providing headers");
            info.addPeerFail(mAddress);
            close();
            return false;
        }

        if(mLastReceiveTime != 0 && time - mLastReceiveTime > 1200)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Not responding");
            info.addPeerFail(mAddress);
            close();
            return false;
        }

        if(!info.spvMode && mMemPoolRequested && !mMemPoolReceived &&
           time - mMemPoolRequestedTime > 30)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Didn't provide mempool.");
            mChain->subtractMemPoolRequest();
            info.addPeerFail(mAddress);
            close();
            return false;
        }

        if(mPrepared && isOutgoing() && !mMemPoolRequested &&
          mLastHeaderHash == mChain->lastHeaderHash() &&(info.spvMode ||
          (info.initialBlockDownloadIsComplete() && mChain->isInSync())) &&
          mSentVersionData != NULL && (mSentVersionData->relay || mBloomFilterID != 0) &&
          (info.spvMode || mChain->memPoolRequests() < 5))
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Sending request for mempool");
            Message::Data memPoolMessage(Message::MEM_POOL);
            if(sendMessage(&memPoolMessage))
            {
                mMemPoolRequested = true;
                mChain->addMemPoolRequest();
                mMemPoolRequestedTime = getTime();
                mMemPoolReceived = false;
            }
        }

        return true;
    }

    void Node::expire()
    {
        mLastExpireTime = getTime();

        // Expire announced transactions
        mAnnounceMutex.lock();
        for(NextCash::HashSet::Iterator hash = mAnnounceTransactions.begin();
          hash != mAnnounceTransactions.end();)
        {
            try
            {
                if(mLastExpireTime - dynamic_cast<HashTime *>(*hash)->time > 300)
                    hash = mAnnounceTransactions.eraseDelete(hash);
                else
                    ++hash;
            }
            catch(...)
            {
                hash = mAnnounceTransactions.eraseDelete(hash);
            }
        }
        mAnnounceTransactions.shrink();
        mAnnounceMutex.unlock();

        // Timeout compact blocks.
        for(std::vector<Message::CompactBlockData *>::iterator compact =
          mOutgoingCompactBlocks.begin(); compact != mOutgoingCompactBlocks.end();)
        {
            if(mLastExpireTime - (*compact)->time > 120)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Removing old outgoing compact block : %s",
                  (*compact)->block->header.hash().hex().text());
                delete *compact;
                compact = mOutgoingCompactBlocks.erase(compact);
            }
            else
                ++compact;
        }

        if(!mProcessingCompactTransactions)
        {
            for(std::vector<Message::CompactBlockData *>::iterator compact =
              mIncomingCompactBlocks.begin(); compact != mIncomingCompactBlocks.end();)
            {
                if(mLastExpireTime - (*compact)->time > 60)
                {
                    // Remove from blocks requested
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Removing old incoming compact block : %s",
                      (*compact)->block->header.hash().hex().text());
                    mBlockRequestMutex.lock();
                    for(NextCash::HashList::iterator hash = mBlocksRequested.begin();
                      hash != mBlocksRequested.end(); ++hash)
                        if(*hash == (*compact)->block->header.hash())
                        {
                            mBlocksRequested.erase(hash);
                            ++mBlockDownloadCount;
                            if(mMessageInterpreter.pendingBlockStartTime != 0)
                            {
                                mBlockDownloadTime +=
                                  mLastExpireTime - mMessageInterpreter.pendingBlockStartTime;
                                mBlockDownloadSize += (*compact)->size;
                            }
                            break;
                        }
                    mBlockRequestMutex.unlock();

                    delete *compact;
                    compact = mIncomingCompactBlocks.erase(compact);
                }
                else
                    ++compact;
            }
        }
    }

    bool Node::sendMessage(Message::Data *pData)
    {
        if(!isOpen())
            return false;

        NextCash::Buffer send;
        mMessageInterpreter.write(pData, &send);
        mConnectionMutex.lock();
        bool success = mConnection->send(&send);
        mConnectionMutex.unlock();
        if(success)
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName, "Sent <%s>",
              Message::nameFor(pData->type));
        else
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName, "Failed to send <%s>",
              Message::nameFor(pData->type));
            close(); // Disconnect
        }
        return success;
    }

    bool Node::waitingForHeaderRequests()
    {
        if(!mHeaderRequested.isEmpty())
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Waiting for headers after : %s", mHeaderRequested.hex().text());
            return true;
        }
        else
            return false;
    }

    bool Node::waitingForBlockRequests()
    {
        if(mBlocksRequested.size() > 0)
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Waiting for %d blocks",
              mBlocksRequested.size());
            return true;
        }
        else
            return false;
    }

    bool Node::requestHeaders()
    {
        if(mStopRequested || !isReady() || !isOpen() || isIncoming() ||
          waitingForHeaderRequests() || waitingForBlockRequests())
            return false;

        if(mChain->isInSync() &&
          !mLastHeaderHash.isEmpty() && mLastHeaderHash == mChain->lastHeaderHash())
            return false; // This node is in sync and will announce any new headers.

        NextCash::Hash requestHash;
        if(!mLastHeaderRequested.isEmpty() &&
          mChain->getHash(mChain->headerHeight() - 1, requestHash) &&
          mLastHeaderRequested == requestHash)
            return false; // This node already requested headers at this height.

        Message::GetHeadersData getHeadersData;
        if(!mChain->getReverseHashes(getHeadersData.hashes, 1, 16, 500))
            return false;

        if(getHeadersData.hashes.size() == 0)
            NextCash::Log::add(NextCash::Log::VERBOSE, mName,
              "Sending request for headers from genesis");
        else
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending request for headers after %d : %s", mChain->headerHeight() - 1,
              getHeadersData.hashes.front().hex().text());

        bool success = sendMessage(&getHeadersData);
        if(success)
        {
            if(getHeadersData.hashes.size() == 0)
                mChain->getHash(0, mHeaderRequested);
            else
                mHeaderRequested = getHeadersData.hashes.front();
            mLastHeaderRequested = mHeaderRequested;
            mHeaderRequestTime = getTime();
        }
        return success;
    }

    bool Node::requestBlocks(NextCash::HashList &pList, bool pForceFull)
    {
        if(Info::instance().spvMode || pList.size() == 0 || mStopRequested || !isReady() ||
          !isOpen() || !isOutgoing())
            return false;

        if(mChain->isInSync() && pList.size() == 1 && mLastHeaderHash != pList.front() &&
          mChain->lastHeaderHash() == pList.front())
            return false;

        Message::GetDataData getDataData;
        Message::InventoryHash::Type type = Message::InventoryHash::BLOCK;
        if(!pForceFull && mSendCompactBlocksVersion != 0L && mChain->isInSync())
            type = Message::InventoryHash::COMPACT_BLOCK;

        // Put block hashes into block request message
        for(NextCash::HashList::iterator hash = pList.begin(); hash != pList.end(); ++hash)
            getDataData.inventory .push_back(new Message::InventoryHash(type, *hash));

        bool success = sendMessage(&getDataData);
        if(success)
        {
            mBlockRequestMutex.lock();
            mBlocksRequested.clear();
            for(NextCash::HashList::iterator hash = pList.begin(); hash != pList.end(); ++hash)
                mBlocksRequested.push_back(*hash);
            mBlockRequestTime = getTime();
            mBlockRequestMutex.unlock();
            mChain->markBlocksForNode(pList, mID);
            if(pList.size() == 1)
            {
                if(type == Message::InventoryHash::COMPACT_BLOCK)
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending request for compact block (%d) : %s", mChain->hashHeight(pList.front()),
                      pList.front().hex().text());
                else
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending request for block (%d) : %s",
                      mChain->hashHeight(pList.front()), pList.front().hex().text());
            }
            else
            {
                if(type == Message::InventoryHash::COMPACT_BLOCK)
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending request for %d compact blocks starting with (%d) : %s", pList.size(),
                      mChain->hashHeight(pList.front()), pList.front().hex().text());
                else
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending request for %d blocks starting with (%d) : %s",
                      pList.size(), mChain->hashHeight(pList.front()), pList.front().hex().text());
            }
        }
        else
        {
            // Clear requested blocks
            mBlockRequestMutex.lock();
            mBlocksRequested.clear();
            mBlockRequestMutex.unlock();
        }

        return success;
    }

    bool Node::sendBloomFilter()
    {
        if(!Info::instance().spvMode || mMonitor == NULL)
            return false;

        Message::FilterLoadData message;
        mBloomFilterID = mMonitor->setupBloomFilter(message.filter);
        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
          "Sending bloom filter with %d bytes and %d functions", message.filter.size(),
          message.filter.functionCount());
        return sendMessage(&message);
    }

    bool Node::requestMerkleBlock(NextCash::Hash &pHash)
    {
        Message::GetDataData message;
        message.inventory
          .push_back(new Message::InventoryHash(Message::InventoryHash::FILTERED_BLOCK, pHash));
        return sendMessage(&message);
    }

    bool Node::requestTransactions(NextCash::HashList &pList, bool pReMark)
    {
        if(pList.size() == 0 || !isReady() || !isOpen() || !isOutgoing())
            return false;

        if(pReMark)
            mChain->memPool().markTransactions(pList, mID);

        // Put transaction hashes into transaction request message
        Message::GetDataData message;
        for(NextCash::HashList::iterator hash = pList.begin(); hash != pList.end(); ++hash)
            if(!mTransactionsRequested.contains(*hash))
            {
                mTransactionsRequested.insert(new HashTime(*hash));
                message.inventory
                  .push_back(new Message::InventoryHash(Message::InventoryHash::TRANSACTION, *hash));
                // NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  // "Sending request for transaction : %s", hash->hex().text());
            }

        if(message.inventory.size() > 0)
            return sendMessage(&message);
        else
            return true; // Only return false on send failure.
    }

    bool Node::requestPeers()
    {
        if(mPeersRequested || isIncoming())
            return false;
        mPeersRequested = true;
        NextCash::Log::add(NextCash::Log::INFO, mName, "Sending peer request");
        Message::Data getAddresses(Message::GET_ADDRESSES);
        return sendMessage(&getAddresses);
    }

    bool Node::sendBlock(BlockReference &pBlock)
    {
        if(!isOpen())
            return false;

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Sending block (%d) (%d KB) : %s",
          mChain->hashHeight(pBlock->header.hash()), pBlock->size() / 1000,
          pBlock->header.hash().hex().text());

        Message::BlockData blockData(pBlock);
        bool success = sendMessage(&blockData);
        if(success)
            ++mStatistics.blocksSent;
        return success;
    }

    bool Node::sendMerkleBlock(BlockReference &pBlock)
    {
        TransactionList includedTransactions;
        Message::MerkleBlockData merkleMessage(pBlock, mFilter, includedTransactions);
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
          "Sending merkle block %d with %d trans : %s", mChain->hashHeight(pBlock->header.hash()),
          includedTransactions.size(), pBlock->header.hash().hex().text());
        if(!sendMessage(&merkleMessage))
            return false;

        Message::TransactionData transactionMessage;
        for(TransactionList::iterator trans = includedTransactions.begin();
          trans != includedTransactions.end(); ++trans)
        {
            transactionMessage.transaction = *trans;
            if(!sendMessage(&transactionMessage))
                return false;
        }

        return true;
    }

    void Node::processBlocksToAnnounce()
    {
        BlockReference block;
        mAnnounceBlockMutex.lock();
        if(mBlocksToAnnounce.size() > 0)
        {
            block = mBlocksToAnnounce.front();
            mBlocksToAnnounce.erase(mBlocksToAnnounce.begin());
        }
        mAnnounceBlockMutex.unlock();

        if(!block)
            return;

        // Process any transactions in the block
        for(TransactionList::iterator trans = block->transactions.begin() + 1;
          trans != block->transactions.end(); ++trans)
        {
            mAnnounceTransactions.remove((*trans)->hash());
            mSavedTransactions.remove((*trans)->hash());
        }

        mAnnounceMutex.lock();
        if(mAnnounceBlocks.contains(block->header.hash()))
        {
            // Don't announce to node that already announced to you
            mAnnounceMutex.unlock();
            return;
        }
        mAnnounceMutex.unlock();

        if(mAnnounceBlocksCompact && mSendCompactBlocksVersion != 0L)
        {
            Message::CompactBlockData *compactBlock = new Message::CompactBlockData(block);
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Announcing block with compact (%d prefilled) : %s", compactBlock->prefilled.size(),
              block->header.hash().hex().text());
            if(sendMessage(compactBlock))
                mOutgoingCompactBlocks.push_back(compactBlock);
            else
                delete compactBlock;
        }
        else if(mSendHeaders)
        {
            // Send the header
            Message::HeadersData *headersData = new Message::HeadersData();
            headersData->headers.push_back(block->header);
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Announcing block with header : %s", block->header.hash().hex().text());
            sendMessage(headersData);
            ++mStatistics.headersSent;
            delete headersData;
        }
        else
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Announcing block with hash : %s", block->header.hash().hex().text());
            Message::InventoryData *inventoryData = new Message::InventoryData();
            inventoryData->inventory.
              push_back(new Message::InventoryHash(Message::InventoryHash::BLOCK, block->header.hash()));
            sendMessage(inventoryData);
            delete inventoryData;
        }
    }

    void Node::announceBlock(BlockReference &pBlock)
    {
        if(!isOpen() || mReceivedVersionData == NULL)
            return;

        mAnnounceBlockMutex.lock();
        mBlocksToAnnounce.push_back(pBlock);
        mAnnounceBlockMutex.unlock();
    }

    void Node::addTransactionAnnouncements(TransactionList &pTransactions)
    {
        if(!isOpen() || !isReady() || mReceivedVersionData == NULL ||
          (!mReceivedVersionData->relay && mFilter.isEmpty()))
            return;

        if(mInventoryData == NULL)
            mInventoryData = new Message::InventoryData();

        mAnnounceMutex.lock();

        for(TransactionList::iterator trans = pTransactions.begin(); trans != pTransactions.end();
          ++trans)
            if(!mAnnounceTransactions.remove((*trans)->hash())) // Hash was not announced to us yet.
            {
                if(mReceivedVersionData->relay)
                {
                    if((*trans)->feeRate() >= mMinimumFeeRate)
                        mInventoryData->inventory.push_back(
                          new Message::InventoryHash(Message::InventoryHash::TRANSACTION,
                          (*trans)->hash()));
                }
                else if(mFilter.contains(*trans))
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Bloom filter contains transaction : %s", (*trans)->hash().hex().text());

                    // Update filter
                    if(mFilter.flags() & BloomFilter::UPDATE_MASK)
                    {
                        ScriptInterpreter::ScriptType type;
                        NextCash::HashList hashes;
                        Outpoint outpoint;

                        outpoint.transactionID = (*trans)->hash();
                        outpoint.index = 0;

                        for(std::vector<Output>::iterator output = (*trans)->outputs.begin();
                          output != (*trans)->outputs.end(); ++output, ++outpoint.index)
                            if(mFilter.containsScript(output->script))
                            {
                                if(mFilter.flags() & BloomFilter::UPDATE_P2PUBKEY_ONLY)
                                {
                                    // Don't add unless P2PKH or MultiSig
                                    type = ScriptInterpreter::parseOutputScript(output->script,
                                      hashes);
                                    if(type != ScriptInterpreter::P2PKH &&
                                      type != ScriptInterpreter::MULTI_SIG)
                                        continue;
                                }

                                // Add new UTXO to filter
                                mFilter.add(outpoint);
                            }
                    }

                    mInventoryData->inventory
                      .push_back(new Message::InventoryHash(Message::InventoryHash::TRANSACTION,
                      (*trans)->hash()));
                }
            }

        mAnnounceMutex.unlock();
    }

    bool Node::finalizeAnnouncments()
    {
        if(mInventoryData == NULL || mInventoryData->inventory.size() == 0)
            return true;

        NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName, "Announcing %d transactions",
          mInventoryData->inventory.size());
        mMessagesToSendLock.lock();
        mMessagesToSend.push_back(mInventoryData);
        mMessagesToSendLock.unlock();
        mInventoryData = NULL;
        return true;
    }

    bool Node::sendTransaction(TransactionReference &pTransaction)
    {
        if(!isOpen() || mReceivedVersionData == NULL || mSentTransactions.contains(pTransaction->hash()))
            return false;

        bool filterContains = mFilter.contains(*pTransaction);

        if(!mReceivedVersionData->relay && !filterContains)
            return false;

        Message::TransactionData transactionData(pTransaction);
        bool result = sendMessage(&transactionData);

        if(result)
        {
            mSentTransactions.push_back(pTransaction->hash());
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Sent transaction : %s", pTransaction->hash().hex().text());
        }
        else
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Failed to send transaction : %s", pTransaction->hash().hex().text());

        return result;
    }

    bool Node::sendVersion()
    {
        if(!isOpen())
            return false;

        Info &info = Info::instance();
        if(mSentVersionData != NULL)
            delete mSentVersionData;
        mSentVersionData = new Message::VersionData(mConnection->ip(), mServices, info.ip,
          info.spvMode, mChain->blockHeight(),
          (isOutgoing() && info.initialBlockDownloadIsComplete() && mChain->isInSync()));
        bool success = sendMessage(mSentVersionData);
        mVersionSent = true;
        return success;
    }

    bool Node::sendPing()
    {
        Milliseconds time = getTimeMilliseconds();
        if(time - mLastPingTime < 60000)
            return true;
        Message::PingData pingData;
        bool success = sendMessage(&pingData);
        if(success)
        {
            NextCash::Log::add(NextCash::Log::DEBUG, mName, "Sent ping");
            mLastPingNonce = pingData.nonce;
            mLastPingTime = time;
        }
        return success;
    }

    bool Node::sendFeeFilter()
    {
        // Get all fee rates and evaluate whether to keep/propagate them.
        // Message::FeeFilterData feeData;
        // feeData.minimumFeeRate = Info::instance().minFee;
        // return sendMessage(&feeData);
        return true;
    }

    bool Node::sendReject(const char *pCommand, Message::RejectData::Code pCode,
      const char *pReason)
    {
        if(!isOpen())
            return false;

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Sending reject : %s", pReason);
        Message::RejectData rejectMessage(pCommand, pCode, pReason, NULL);
        return sendMessage(&rejectMessage);
    }

    bool Node::sendRejectWithHash(const char *pCommand, Message::RejectData::Code pCode,
      const char *pReason, const NextCash::Hash &pHash)
    {
        if(!isOpen())
            return false;

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Sending reject : %s", pReason);
        Message::RejectData rejectMessage(pCommand, pCode, pReason, NULL);
        pHash.write(&rejectMessage.extra);
        return sendMessage(&rejectMessage);
    }

    bool Node::initialize()
    {
        mConnectionMutex.lock();

        bool isNewConnection = false;
        if(mConnection == NULL)
        {
            isNewConnection = true;
            mConnection = new NextCash::Network::Connection(mAddress, 10);
        }
        else if(!isIncoming())
            mAddress = mConnection->ip();

        // Verify connection
        if(!mConnection->isOpen())
        {
            mConnectionMutex.unlock();
            mStopRequested = true;
            mStopped = true;
            if(!isIncoming())
                Info::instance().addPeerFail(mAddress, 1, 1);
            mIsInitialized = true;
            return false;
        }
        mConnected = true;
        mConnectionMutex.unlock();

        if(isNewConnection)
            mDaemon->registerConnection(mConnectionType);

        if(isIncoming())
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Incoming Connection %s", mConnection->ip().text().text());
        else
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Outgoing Connection %s", mConnection->ip().text().text());

        mIsInitialized = true;
        return true;
    }

    void Node::runInThread()
    {
        mStarted = true;
        if(!initialize())
        {
            mStopped = true;
            release();
            return;
        }

        if(mStopRequested || (mStopFlag != NULL && *mStopFlag))
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Node stopped before thread started");
            mStopped = true;
            return;
        }

        while(true)
        {
            process();

            if(mStopRequested || (mStopFlag != NULL && *mStopFlag))
                break;

            NextCash::Thread::sleep(100);
        }

        mStopped = true;
        release();
    }

    void Node::run(void *pParameter)
    {
        Node *node = (Node *)pParameter;
        if(node == NULL)
        {
            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_NODE_LOG_NAME,
              "Thread parameter is null. Stopping");
            return;
        }

        node->runInThread();
    }

    void Node::addAnnouncedBlock(const NextCash::Hash &pHash)
    {
        mAnnounceMutex.lock();
        if(!mAnnounceBlocks.contains(pHash))
        {
            // Keep list at 1024 or less
            if(mAnnounceBlocks.size() > 1024)
                mAnnounceBlocks.erase(mAnnounceBlocks.begin());
            mAnnounceBlocks.push_back(pHash);
            mLastHeaderHash = pHash;
        }
        mAnnounceMutex.unlock();
    }

    bool Node::addAnnouncedTransaction(const NextCash::Hash &pHash)
    {
        mAnnounceMutex.lock();
        HashTime *newHashTime = new HashTime(pHash);
        if(!mAnnounceTransactions.insert(newHashTime))
            delete newHashTime;
        mAnnounceMutex.unlock();

        return mMonitor != NULL && mMonitor->addTransactionAnnouncement(pHash, mID);
    }

    bool Node::checkSaved()
    {
        if(getTime() - mLastSavedCheckTime < 2)
            return true;

        NextCash::HashList toRequest;
        for(NextCash::HashList::iterator hash = mSavedTransactions.begin();
          hash != mSavedTransactions.end();)
            switch(mChain->memPool().hashStatus(*hash, mID, true))
            {
            case MemPool::HASH_NEED: // Failed previous request. Try with this node.
                toRequest.push_back(*hash);
                hash = mSavedTransactions.erase(hash);
                break;
            case MemPool::HASH_REQUESTED: // Still requested by another node.
                ++hash;
                break;
            default: // Already received.
                hash = mSavedTransactions.erase(hash);
                break;
            }

        if(toRequest.size() > 0)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Re-requested %d transactions", toRequest.size());
            if(!requestTransactions(toRequest, false))
                return false;
        }

        mLastSavedCheckTime = getTime();
        return true;
    }

    Node::FillResult Node::fillCompactBlock(Message::CompactBlockData *pCompactBlock,
      bool pRequestTransactions)
    {
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_NODE_FILL_COMPACT_ID, PROFILER_NODE_FILL_COMPACT_NAME), true);
#endif
        Message::GetCompactTransData getTransactions(pCompactBlock->block->header.hash());

        pCompactBlock->block->transactions.clear();
        pCompactBlock->block->transactions.resize(pCompactBlock->block->header.transactionCount,
          NULL);

        NextCash::SortedSet memPoolShortIDs;
        ShortIDHash *memPoolShortID, shortIDLookup;
        mChain->memPool().calculateShortIDs(pCompactBlock, memPoolShortIDs);

        unsigned int encodedOffset = 1; // First offset will subtract 1 to zero
        unsigned int offset = 0;
        unsigned int i;
        NextCash::stream_size increasedSize = 0L;
        NextCash::SortedSet shortIDs;
        std::vector<uint64_t>::iterator shortID = pCompactBlock->shortIDs.begin();
        TransactionList::iterator trans = pCompactBlock->block->transactions.begin();

        for(std::vector<Message::PrefilledTransaction>::iterator prefilled =
          pCompactBlock->prefilled.begin(); prefilled != pCompactBlock->prefilled.end();
          ++prefilled)
        {
            for(i = 0; i < prefilled->offset; ++i)
            {
                // Use short IDs to fill the gap between prefilled.
                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  // "Short ID %d 0x%08x%08x", offset, *shortID >> 32, *shortID & 0xffffffff);
                shortIDLookup.shortID = *shortID;
                memPoolShortID = (ShortIDHash *)memPoolShortIDs.get(shortIDLookup);
                if(memPoolShortID != NULL)
                    *trans = mChain->memPool().getTransaction(memPoolShortID->hash);

                if(!*trans)
                {
                    // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      // "Compact block missing transaction %d (encoded %d) : 0x%04x%08x", offset,
                      // encodedOffset - 1, *shortID >> 32, *shortID & 0x00000000ffffffff);
                    if(!pRequestTransactions)
                    {
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Abandoning non-full compact block : %s",
                          pCompactBlock->block->header.hash().hex().text());
                        pCompactBlock->block->setSize(pCompactBlock->block->size() +
                          increasedSize);
                        return FILL_ABANDONED;
                    }
                    getTransactions.offsets.emplace_back(encodedOffset - 1);
                    encodedOffset = 0;
                }
                else
                {
                    // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      // "Compact block found transaction %d : 0x%04x%08x : %s", offset, *shortID >> 32,
                      // *shortID & 0x00000000ffffffff, (*trans)->hash().hex().text());
                    increasedSize += (*trans)->size();
                }

                ++shortID;
                ++trans;
                ++offset;
                ++encodedOffset;
            }

            // Use next prefilled transaction.
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              // "Prefilled %d : %s", offset, prefilled->transaction->hash.hex().text());
            *trans = prefilled->transaction;
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              // "Compact block provided transaction %d : %s", offset, (*trans)->hash().hex().text());
            increasedSize += (*trans)->size();
            prefilled->transaction = NULL;

            ++trans;
            ++offset;
            ++encodedOffset;
        }

        while(shortID != pCompactBlock->shortIDs.end())
        {
            // Use short IDs to finish.
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              // "Short ID %d 0x%08x%08x", offset, *shortID >> 32, *shortID & 0xffffffff);
            shortIDLookup.shortID = *shortID;
            memPoolShortID = (ShortIDHash *)memPoolShortIDs.get(shortIDLookup);
            if(memPoolShortID != NULL)
                *trans = mChain->memPool().getTransaction(memPoolShortID->hash);

            if(!*trans)
            {
                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  // "Compact block missing transaction %d (encoded %d) : 0x%04x%08x", offset,
                  // encodedOffset - 1, *shortID >> 32, *shortID & 0x00000000ffffffff);
                if(!pRequestTransactions)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Abandoning non-full compact block : %s",
                      pCompactBlock->block->header.hash().hex().text());
                    pCompactBlock->block->setSize(pCompactBlock->block->size() + increasedSize);
                    return FILL_ABANDONED;
                }
                getTransactions.offsets.emplace_back(encodedOffset - 1);
                encodedOffset = 0;
            }
            else
            {
                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  // "Compact block found transaction %d : 0x%04x%08x : %s", offset, *shortID >> 32,
                  // *shortID & 0x00000000ffffffff, (*trans)->hash().hex().text());
                increasedSize += (*trans)->size();
            }

            ++shortID;
            ++trans;
            ++offset;
            ++encodedOffset;
        }

        pCompactBlock->block->setSize(pCompactBlock->block->size() + increasedSize);

        if(getTransactions.offsets.size() > 0) // Missing transactions
        {
            unsigned int percentMissing = ((double)getTransactions.offsets.size() /
              (double)pCompactBlock->block->transactions.size()) * 100.0;
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Compact block missing %d/%d (%d%%) transactions", getTransactions.offsets.size(),
              pCompactBlock->block->transactions.size(), percentMissing);
            if(percentMissing > 90)
                return FILL_FAILED;
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Requesting %d compact block transactions : %s", getTransactions.offsets.size(),
                  pCompactBlock->block->header.hash().hex().text());
                sendMessage(&getTransactions);
                return FILL_INCOMPLETE; // Wait for transactions
            }
        }
        else // Block full
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Compact block is full : %s", pCompactBlock->block->header.hash().hex().text());
            return FILL_COMPLETE;
        }
    }

    bool Node::addTransactionsToCompactBlock(Message::CompactBlockData *pData,
      Message::CompactTransData *pTransData)
    {
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_NODE_COMPACT_ADD_TRANS_ID, PROFILER_NODE_COMPACT_ADD_TRANS_NAME), true);
#endif
        std::vector<uint64_t> givenShortIDs;
        TransactionList::iterator givenTrans;
        givenShortIDs.reserve(pTransData->transactions.size());
        for(givenTrans = pTransData->transactions.begin();
          givenTrans != pTransData->transactions.end(); ++givenTrans)
        {
            givenShortIDs.emplace_back(pData->calculateShortID((*givenTrans)->hash()));
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              // "Compact block transaction given 0x%04x%08x : %s", givenShortIDs.back() >> 32,
              // givenShortIDs.back() & 0x00000000ffffffff, (*givenTrans)->hash.hex().text());
        }

        bool found;
        unsigned int offset = 0;
        unsigned int i;
        NextCash::stream_size increasedSize = 0L;
        std::vector<uint64_t>::iterator shortID = pData->shortIDs.begin();
        TransactionList::iterator trans = pData->block->transactions.begin();

        for(std::vector<Message::PrefilledTransaction>::iterator prefilled =
          pData->prefilled.begin();
          prefilled != pData->prefilled.end(); ++prefilled)
        {
            for(i = 0; i < prefilled->offset; ++i)
            {
                // Use short IDs to fill the gap between prefilled.
                if(!*trans)
                {
                    // Find short ID in given transactions.
                    found = false;
                    for(std::vector<uint64_t>::iterator givenShortID = givenShortIDs.begin();
                      givenShortID != givenShortIDs.end(); ++givenShortID)
                        if(*givenShortID == *shortID)
                        {
                            givenTrans = pTransData->transactions.begin() +
                              (givenShortID - givenShortIDs.begin());

                            if(!*givenTrans)
                            {
                                NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                                  "Compact block transaction already used %d : 0x%04x%08x", offset,
                                  *givenShortID >> 32, *givenShortID & 0x00000000ffffffff);
                            }
                            else
                            {
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Compact block transaction found %d : 0x%04x%08x : %s", offset,
                                  // *givenShortID >> 32, *givenShortID & 0x00000000ffffffff,
                                  // (*givenTrans)->hash.hex().text());
                                *trans = *givenTrans;
                                pTransData->transactions.erase(givenTrans);
                                givenShortIDs.erase(givenShortID);
                                increasedSize += (*trans)->size();
                                found = true;
                            }
                            break;
                        }

                    if(!found)
                    {
                        NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                          "Compact block transaction not found 0x%04x%08x",
                          *shortID >> 32, *shortID & 0x00000000ffffffff);
                        pData->block->setSize(pData->block->size() + increasedSize);
                        return false;
                    }
                }

                ++shortID;
                ++trans;
                ++offset;
            }

            // Skip prefilled transactions.
            if(!*trans)
            {
                // This should not happen
                NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                  "Prefilled transaction %d : missing", offset);
                pData->block->setSize(pData->block->size() + increasedSize);
                return false;
            }

            ++trans;
            ++offset;
        }

        while(shortID != pData->shortIDs.end())
        {
            // Use short IDs to finish.
            if(!*trans)
            {
                // Find short ID in given transactions.
                found = false;
                for(std::vector<uint64_t>::iterator givenShortID = givenShortIDs.begin();
                  givenShortID != givenShortIDs.end(); ++givenShortID)
                    if(*givenShortID == *shortID)
                    {
                        givenTrans = pTransData->transactions.begin() +
                          (givenShortID - givenShortIDs.begin());

                        if(!*givenTrans)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                              "Compact block transaction already used %d : 0x%04x%08x", offset,
                              *givenShortID >> 32, *givenShortID & 0x00000000ffffffff);
                        }
                        else
                        {
                            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              // "Compact block transaction found %d : 0x%04x%08x : %s", offset,
                              // *givenShortID >> 32, *givenShortID & 0x00000000ffffffff,
                              // (*givenTrans)->hash.hex().text());
                            *trans = *givenTrans;
                            pTransData->transactions.erase(givenTrans);
                            givenShortIDs.erase(givenShortID);
                            increasedSize += (*trans)->size();
                            found = true;
                        }
                        break;
                    }

                if(!found)
                {
                    NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                      "Compact block transaction not found %d : 0x%04x%08x", offset,
                      *shortID >> 32, *shortID & 0x00000000ffffffff);
                    pData->block->setSize(pData->block->size() + increasedSize);
                    return false;
                }
            }

            ++shortID;
            ++trans;
            ++offset;
        }

        pData->block->setSize(pData->block->size() + increasedSize);

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
          "Compact block (with trans) is full : %s", pData->block->header.hash().hex().text());
        return true;
    }

    void Node::process()
    {
        if(!mConnected || mStopRequested)
            return;

        mConnectionMutex.lock();
        mReceiveBuffer.compact();

        if(mConnection == NULL)
        {
            mConnectionMutex.unlock();
            return;
        }

        if(!mConnection->isOpen())
        {
            mConnected = false;
            mConnectionMutex.unlock();
            close();
            return;
        }

        // Receive
        NextCash::stream_size previousBufferOffset = mReceiveBuffer.remaining();

        try
        {
            mConnection->receive(&mReceiveBuffer);
        }
        catch(std::bad_alloc pException)
        {
            mConnectionMutex.unlock();
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Bad allocation while receiving data : %s", pException.what());
            close();
            return;
        }
        catch(std::exception pException)
        {
            mConnectionMutex.unlock();
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Exception while receiving data : %s", pException.what());
            close();
            return;
        }

        mConnectionMutex.unlock();

        processBlocksToAnnounce();

        // Send
        mMessagesToSendLock.lock();
        std::vector<Message::Data *> messagesToSend(mMessagesToSend);
        mMessagesToSend.clear();
        mMessagesToSendLock.unlock();

        for(std::vector<Message::Data *>::iterator message = messagesToSend.begin();
          message != messagesToSend.end(); ++message)
        {
            sendMessage(*message);
            delete *message;
        }

        if(!mVersionSent)
            sendVersion();

        if(mMessagesReceived > PEER_MESSAGE_LIMIT)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Reached message limit");
            close();
            return;
        }

        Time time = getTime();
        if(time - mConnectedTime > PEER_TIME_LIMIT)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Reached time limit");
            close();
            return;
        }

        if(time - mLastCheckTime > 5 && !check())
            return; // Closed

        if(time - mLastExpireTime > 60)
            expire();

        // if(time - mLastBlackListCheck > 10)
        // {
            // mLastBlackListCheck = time;
            // for (NextCash::HashList::iterator hash = mAnnounceTransactions.begin();
                 // hash != mAnnounceTransactions.end(); ++hash)
                // if (mChain->memPool().isBlackListed(*hash))
                // {
                    // NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                      // "Dropping. Detected black listed transaction : %s", hash->hex().text());
                    // info.addPeerFail(mAddress);
                    // close();
                    // return;
                // }
        // }

        if(previousBufferOffset != mReceiveBuffer.remaining())
        {
            // Prevent looping here too long when getting lots of messages.
            unsigned int messageCount = 0;
            while(++messageCount < 20 && processMessage());
        }

        Info &info = Info::instance();

        if(!info.spvMode && !checkSaved())
            return;

#ifdef SINGLE_THREAD
        if(mMessagesReceived == 0 && time - mConnectedTime > 60)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Dropping. No valid messages within 60 seconds of connecting %d bytes received.",
              mConnection->bytesReceived() + mStatistics.bytesReceived);
#else
        if(mMessagesReceived == 0 && time - mConnectedTime > 10)
        {
            mConnectionMutex.lock();
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Dropping. No valid messages within 10 seconds of connecting. %d bytes received.",
              (unsigned int)mConnection->bytesReceived() + mStatistics.bytesReceived);
            mConnectionMutex.unlock();
#endif
            close();
            info.addPeerFail(mAddress, 5);
            return;
        }

        if(info.initialBlockDownloadIsComplete() && isOutgoing() && info.spvMode && isReady() &&
          isOpen() && mMonitor != NULL && time - mLastMerkleCheck > 2)
        {
            if(mMonitor->needsClose(mID))
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Monitor requested.");
                close();
                return;
            }

            if(mMonitor->filterNeedsResend(mID, mBloomFilterID))
                sendBloomFilter();

            if(mActiveMerkleRequests < 5)
            {
                bool fail = false;
                NextCash::HashList blockHashes;

                mMonitor->getNeededMerkleBlocks(mID, *mChain, blockHashes, 25);

                if(blockHashes.size() == 0)
                {
                    if(mMonitor->height() < mChain->headerHeight())
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "No merkle blocks available to request", blockHashes.size());
                }
                else
                {
                    for(NextCash::HashList::iterator hash = blockHashes.begin();
                      hash != blockHashes.end(); ++hash)
                        if(!requestMerkleBlock(*hash))
                        {
                            fail = true;
                            break;
                        }

                    if(!fail)
                    {
                        mActiveMerkleRequests += blockHashes.size();
                        mLastMerkleRequest = getTime();
                        mLastMerkleReceive = mLastMerkleRequest;
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Requested %d merkle blocks", blockHashes.size());
                    }
                }
            }
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Waiting for %d merkle blocks from %ds ago", mActiveMerkleRequests,
                  time - mLastMerkleRequest);
                if(time - mLastMerkleReceive > 60)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Took too long to return merkle blocks");
                    close();
                    return;
                }
            }

            mLastMerkleCheck = time;
        }

        if(mLastPingTime != 0 && mPingRoundTripTime == 0xffffffffffffffff &&
           (getTimeMilliseconds() - mLastPingTime) / 1000L > mPingCutoff)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Dropping. Ping not received within cutoff of %ds", mPingCutoff);
            info.addPeerFail(mAddress);
            close();
            return;
        }
    }

    bool Node::failedStartBytes()
    {
        // Start String
        const uint8_t *startBytes = networkStartBytes();
        unsigned int matchOffset = 0;
        NextCash::stream_size startReadOffset = mReceiveBuffer.readOffset();

        // Search for start string
        while(mReceiveBuffer.remaining())
        {
            if(mReceiveBuffer.readByte() == startBytes[matchOffset])
            {
                matchOffset++;
                if(matchOffset == 4)
                    break;
            }
            else
                return true;
        }

        mReceiveBuffer.setReadOffset(startReadOffset);
        return false;
    }

    bool Node::updateBlockRequest(const NextCash::Hash &pHash, Message::Data *pMessage,
      bool pComplete)
    {
        bool found = false;
        mBlockRequestMutex.lock();
        for(NextCash::HashList::iterator hash = mBlocksRequested.begin();
          hash != mBlocksRequested.end(); ++hash)
            if(*hash == pHash)
            {
                found = true;
                if(pComplete)
                {
                    mBlocksRequested.erase(hash);
                    ++mBlockDownloadCount;
                }
                mLastBlockReceiveTime = getTime();
                if(mMessageInterpreter.pendingBlockStartTime != 0)
                {
                    mBlockDownloadTime +=
                      mLastBlockReceiveTime - mMessageInterpreter.pendingBlockStartTime;
                    mBlockDownloadSize += pMessage->size;
                }
                break;
            }
        mBlockRequestMutex.unlock();
        return found;
    }

    bool Node::processMessage()
    {
        if(mMessagesReceived > PEER_MESSAGE_LIMIT && !mStopRequested)
            return false;

        // Check for a complete message
        Message::Data *message;
        bool dontDeleteMessage = false;
        bool success = true;
        Time time = getTime();
        Info &info = Info::instance();

        if(mMessagesReceived == 0 && failedStartBytes())
        {
            mRejected = true;
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Dropping. Invalid start bytes");
            close();
            info.addPeerFail(mAddress, 5);
            return false;
        }

        try
        {
            message = mMessageInterpreter.read(&mReceiveBuffer, mName);
        }
        catch(std::bad_alloc pException)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Bad allocation while reading message : %s", pException.what());
            close();
            return false;
        }
        catch(std::exception pException)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Exception while reading message : %s", pException.what());
            close();
            return false;
        }

        if(message == NULL)
        {
            if(time - mLastReceiveTime > 600) // 10 minutes
                sendPing();

            if(!mMessageInterpreter.pendingBlockHash.isEmpty() &&
              mMessageInterpreter.pendingBlockUpdateTime != 0)
                mChain->updateBlockProgress(mMessageInterpreter.pendingBlockHash, mID,
                  mMessageInterpreter.pendingBlockUpdateTime);

            return false;
        }

        NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName, "Received <%s>",
          Message::nameFor(message->type));
        mLastReceiveTime = time;

        if(mMessagesReceived < 2 && message->type != Message::VERSION &&
          message->type != Message::VERACK && message->type != Message::REJECT)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "First 2 messages not a version and verack : <%s>",
              Message::nameFor(message->type));
            close();
            if(!isSeed())
                info.addPeerFail(mAddress);
            delete message;
            return false;
        }

        ++mMessagesReceived;

        switch(message->type)
        {
        case Message::VERSION:
        {
            if(mReceivedVersionData != NULL)
            {
                sendReject(Message::nameFor(message->type), Message::RejectData::DUPLICATE,
                  "Dropping. More than one version message");
                info.addPeerFail(mAddress);
                close();
                break;
            }

            mReceivedVersionData = (Message::VersionData *)message;
            dontDeleteMessage = true;

            NextCash::String timeText;
            timeText.writeFormattedTime(mReceivedVersionData->time);
            NextCash::String versionText;
            versionText.writeFormatted("Version : %s (%d), %d blocks",
              mReceivedVersionData->userAgent.text(), mReceivedVersionData->version,
              mReceivedVersionData->startBlockHeight);
            if(mReceivedVersionData->relay)
                versionText += ", relay";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::FULL_NODE_BIT)
                versionText += ", full";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::CASH_NODE_BIT)
                versionText += ", cash";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::BLOOM_NODE_BIT)
                versionText += ", bloom";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::GETUTXO_NODE_BIT)
                versionText += ", get utxo";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::WITNESS_NODE_BIT)
                versionText += ", witness";
            if(mReceivedVersionData->transmittingServices & Message::VersionData::XTHIN_NODE_BIT)
                versionText += ", xthin";
            versionText += ", time ";
            versionText += timeText;
            NextCash::Log::add(NextCash::Log::INFO, mName, versionText);

            mAddress.set(NextCash::Network::IPAddress::IPV6,
              mReceivedVersionData->transmittingIPv6, mReceivedVersionData->transmittingPort);
            mMessageInterpreter.version = mReceivedVersionData->version;

            if(!mAddress.isValid() || mAddress.port() == 0)
                mAddress = mConnection->ip();

            info.updatePeer(mAddress, mReceivedVersionData->userAgent,
              mReceivedVersionData->transmittingServices);

            if(isOutgoing())
            {
//                    if(!(mReceivedVersionData->transmittingServices &
//                      Message::VersionData::FULL_NODE_BIT))
//                    {
//                        // Require full node bit for outgoing nodes
//                        sendReject(Message::nameFor(message->type),
//                          Message::RejectData::PROTOCOL,
//                          "Full node bit (0x01) required in protocol version");
//                        NextCash::Log::add(NextCash::Log::INFO, mName,
//                          "Dropping. Missing full node bit");
//                        info.addPeerFail(mAddress);
//                        close();
//                        success = false;
//                        break;
//                    }
//                    else
                if(mReceivedVersionData->startBlockHeight + 2000 < mChain->headerHeight())
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Low block height (%d)",
                      mReceivedVersionData->startBlockHeight);
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                    break;
                }
                else if(info.spvMode && !(mReceivedVersionData->transmittingServices &
                  Message::VersionData::BLOOM_NODE_BIT))
                {
                    sendReject(Message::nameFor(message->type),
                      Message::RejectData::PROTOCOL,
                      "Bloom node bit (0x04) required in protocol version");
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. Missing bloom node bit");
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                    break;
                }
            }

            // Send version acknowledge
            Message::Data versionAcknowledgeMessage(Message::VERACK);
            sendMessage(&versionAcknowledgeMessage);
            mVersionAcknowledgeSent = true;

            if(mVersionAcknowledged)
                prepare();
            break;
        }
        case Message::VERACK:
            mVersionAcknowledged = true;
            if(mReceivedVersionData != NULL)
                prepare();
            break;
        case Message::PING:
        {
            ++mPingCount;
            Message::PongData pongData(((Message::PingData *)message)->nonce);
            sendMessage(&pongData);

            if(mPingCount > 100)
            {
                NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Reached ping limit");
                close();
                success = false;
            }
            break;
        }
        case Message::PONG:
            NextCash::Log::add(NextCash::Log::DEBUG, mName, "Received pong");
            if(((Message::PongData *)message)->nonce != 0 &&
              mLastPingNonce != ((Message::PongData *)message)->nonce)
            {
                NextCash::Log::add(NextCash::Log::INFO, mName,
                  "Dropping. Pong nonce doesn't match sent Ping");
                close();
                success = false;
            }
            else
            {
                if(mPingRoundTripTime == 0xffffffffffffffff)
                {
                     NextCash::Log::add(NextCash::Log::DEBUG, mName,
                       "Received round trip pong");
                    mPingRoundTripTime = getTimeMilliseconds() - mLastPingTime;
                    if(!isIncoming())
                    {
                        if(mPingRoundTripTime / 1000L > mPingCutoff)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Dropping. Ping time %dms not within cutoff of %ds",
                              mPingRoundTripTime, mPingCutoff);
                            close();
                            success = false;
                        }
                        else
                            prepare();
                    }
                }
                mLastPingNonce = 0;
            }
            break;
        case Message::REJECT:
        {
            Message::RejectData *rejectData = (Message::RejectData *)message;
            if(rejectData->command == "version")
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Closing for version reject [%02x] - %s", rejectData->code,
                  rejectData->reason.text());
                close();
            }
            else if((rejectData->command == "tx" || rejectData->command == "block") &&
              rejectData->extra.length() >= 32)
            {
                NextCash::Hash hash(32);
                hash.read(&rejectData->extra);
                NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                  "Reject %s [%02x] - %s : %s", rejectData->command.text(), rejectData->code,
                  rejectData->reason.text(), hash.hex().text());
            }
            else
                NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                  "Reject %s [%02x] - %s", rejectData->command.text(), rejectData->code,
                  rejectData->reason.text());

            // if(rejectData->code == Message::RejectData::LOW_FEE)
            //   Possibly look up transaction and set minimum fee filter above rate of
            //     transaction that was rejected

            // TODO Determine if closing node is necessary
            break;
        }
        case Message::GET_ADDRESSES:
        {
            // Send known peer addresses
            Message::AddressesData addressData;
            std::vector<Peer *> peers;
            uint64_t servicesMask = Message::VersionData::FULL_NODE_BIT;

            // Get list of peers
            info.getRandomizedPeers(peers, 1, servicesMask, info.chainID);

            unsigned int count = peers.size();
            if(count > 1000) // Maximum of 1000
                count = 1000;
            if(count == 0)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "No peer addresses available to send");
                break;
            }

            // Add peers to message
            addressData.addresses.resize(count);
            std::vector<Peer *>::iterator peer = peers.begin();
            for(std::vector<Message::Address>::iterator toSend = addressData.addresses.begin();
              toSend != addressData.addresses.end(); ++toSend)
                *toSend = **peer++;

            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending %d peer addresses", addressData.addresses.size());
            sendMessage(&addressData);
            break;
        }
        case Message::ADDRESSES:
            // Don't accept peer addresses unless the node is on the right chain.
            if(!mLastHeaderHash.isEmpty() && !isIncoming())
            {
                Message::AddressesData *addressesData = (Message::AddressesData *)message;
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Received %d peer addresses", addressesData->addresses.size());

                for(std::vector<Message::Address>::iterator address =
                  addressesData->addresses.begin(); address != addressesData->addresses.end() &&
                  !mStopRequested; ++address)
                    info.addPeer(address->ip, address->services);

                if(isSeed())
                {
                    NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                      "Closing seed because it gave addresses");
                    close(); // Disconnect from seed node because it has done its job
                }
            }
            break;
        case Message::ALERT:
            //TODO Determine if anything needs to be done for alerts
            break;

        case Message::FEE_FILTER:
            mMinimumFeeRate = ((Message::FeeFilterData *)message)->minimumFeeRate;
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Fee minimum rate set to %d", mMinimumFeeRate);
            break;

        case Message::FILTER_ADD:
        {
            // Add data to the bloom filter
            Message::FilterAddData *filterAddData = (Message::FilterAddData *)message;
            mFilter.addData(filterAddData->data);
            break;
        }
        case Message::FILTER_CLEAR:
            mFilter.clear();
            break;

        case Message::FILTER_LOAD:
        {
            // Load a new bloom filter
            Message::FilterLoadData *filterLoadData = (Message::FilterLoadData *)message;
            mFilter.assign(filterLoadData->filter);
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Bloom filter loaded with %d bytes and %d functions", mFilter.size(),
              mFilter.functionCount());
            break;
        }
        case Message::SEND_HEADERS:
            mSendHeaders = true;
            break;
        case Message::GET_BLOCKS:
        {
            // Send Inventory of block headers
            Message::GetBlocksData *getBlocksData = (Message::GetBlocksData *)message;

            // Find appropriate hashes
            NextCash::HashList hashes;
            for(NextCash::HashList::iterator i = getBlocksData->hashes.begin();
              i != getBlocksData->hashes.end(); ++i)
                if(mChain->getHashes(hashes, *i, 500))
                    break;

            if(hashes.size() == 0)
            {
                // No matching starting hashes found. Start from genesis
                NextCash::Hash emptyHash;
                mChain->getHashes(hashes, emptyHash, 500);
            }

            unsigned int count = hashes.size();
            if(count > 500) // Maximum of 500
                count = 500;

            // Add inventory to message
            bool dontStop = getBlocksData->stopHeaderHash.isZero();
            Message::InventoryData inventoryData;
            inventoryData.inventory.resize(count);
            unsigned int actualCount = 0;
            Message::Inventory::iterator item = inventoryData.inventory.begin();
            for(NextCash::HashList::iterator hash = hashes.begin(); hash != hashes.end();
              ++hash)
            {
                *item = new Message::InventoryHash(Message::InventoryHash::BLOCK, *hash);
                ++actualCount;
                ++item;
                if(!dontStop && *hash == getBlocksData->stopHeaderHash)
                    break;
            }
            inventoryData.inventory.resize(actualCount);

            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending %d block hashes", actualCount);
            sendMessage(&inventoryData);
            break;
        }
        case Message::GET_DATA:
        {
            // Don't respond to data requests before receiving the version message
            if(mReceivedVersionData == NULL)
                break;

            Message::GetDataData *getDataData = (Message::GetDataData *)message;
            Message::NotFoundData notFoundData;
            BlockReference block;
            bool fail = false;

            for(Message::Inventory::iterator item = getDataData->inventory.begin();
              item != getDataData->inventory.end() && !mStopRequested; ++item)
            {
                switch((*item)->type)
                {
                case Message::InventoryHash::BLOCK:
                {
                    unsigned int height = mChain->hashHeight((*item)->hash);
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Requested block (%d) : %s", height, (*item)->hash.hex().text());
                    block = mChain->getBlock((*item)->hash);
                    if(block)
                    {
                        if(!sendBlock(block))
                            fail = true;
                    }
                    else
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Block not found : %s", (*item)->hash.hex().text());
                        notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                    }
                    break;
                }
                case Message::InventoryHash::TRANSACTION:
                {
                    TransactionReference transaction =
                      mChain->memPool().getTransaction((*item)->hash);
                    if(transaction)
                    {
                        Message::TransactionData transactionData(transaction);
                        NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                          "Sending transaction (%d bytes) : %s",
                          transactionData.transaction->size(), (*item)->hash.hex().text());
                        sendMessage(&transactionData);
                    }
                    else
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Transaction not found : %s", (*item)->hash.hex().text());
                        notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                    }
                    break;
                }
                case Message::InventoryHash::FILTERED_BLOCK:
                {
                    unsigned int height = mChain->hashHeight((*item)->hash);
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Requested merkle block (%d) : %s", height, (*item)->hash.hex().text());

                    block = mChain->getBlock((*item)->hash);
                    if(block)
                    {
                        if(!sendMerkleBlock(block))
                            fail = true;
                    }
                    else
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Block not found : %s", (*item)->hash.hex().text());
                        notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                    }
                    break;
                }
                case Message::InventoryHash::COMPACT_BLOCK:
                {
                    unsigned int height = mChain->hashHeight((*item)->hash);
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Requested compact block (%d) : %s", height, (*item)->hash.hex().text());

                    block = mChain->getBlock((*item)->hash);
                    if(block)
                    {
                        if(mSendCompactBlocksVersion != 0L &&
                          mChain->headerHeight() - height < 10)
                        {
                            Message::CompactBlockData *compactBlock =
                              new Message::CompactBlockData(block);
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                             "Sending compact block (%d/%d prefilled) (%d) : %s",
                             compactBlock->prefilled.size(),
                             compactBlock->block->transactions.size(), height,
                             compactBlock->block->header.hash().hex().text());
                            if(sendMessage(compactBlock))
                                mOutgoingCompactBlocks.push_back(compactBlock);
                            else
                                delete compactBlock;
                        }
                        else
                        {
                            if(!sendBlock(block))
                                fail = true;
                        }
                    }
                    else
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Block not found : %s", (*item)->hash.hex().text());
                        notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                    }
                    break;
                }
                case Message::InventoryHash::UNKNOWN:
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Unknown request inventory type %02x", (*item)->type);
                    break;
                }

                if(fail)
                    break;
            }

            if(notFoundData.inventory.size() > 0)
                sendMessage(&notFoundData);
            break;
        }
        case Message::GET_HEADERS:
        {
            // Don't respond to header requests before receiving the version message
            if(mReceivedVersionData == NULL)
                break;

            Message::GetHeadersData *getHeadersData = (Message::GetHeadersData *)message;
            Message::HeadersData sendHeadersData;
            unsigned int height;
            bool found = false;
            unsigned int offset = 0;

            for(NextCash::HashList::iterator hash = getHeadersData->hashes.begin();
              hash != getHeadersData->hashes.end(); ++hash)
            {
                height = mChain->hashHeight(*hash);
                if(height != 0xffffffff)
                {
//                        if(height > HISTORY_BRANCH_CHECKING &&
//                          height < (unsigned int)mReceivedVersionData->startBlockHeight -
//                          HISTORY_BRANCH_CHECKING)
//                        {
//                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
//                              "Dropping. Requested header height %d (%d/%d) which is below start block height %d : %s",
//                              height, offset, getHeadersData->hashes.size(),
//                              mReceivedVersionData->startBlockHeight, hash->hex().text());
//                            sendReject(Message::nameFor(Message::GET_HEADERS),
//                              Message::RejectData::WRONG_CHAIN, "Too many unmatching headers");
//                            close();
//                            success = false;
//                            break;
//                        }
//                        else
//                        {
                        NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                          "Headers requested after height %d (%d/%d) : %s", height, offset,
                          getHeadersData->hashes.size(), hash->hex().text());
                        if(height == mChain->headerHeight())
                            found = true; // Don't send any
                        else if(mChain->getHeaders(sendHeadersData.headers, *hash,
                          getHeadersData->stopHeaderHash, 2000))
                            found = true; // Send up to 2000
                        else
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Failed to get headers for header request (%d/%d) : %s", offset,
                              getHeadersData->hashes.size(), hash->hex().text());
                        break; // match found
//                        }
                }

                ++offset;
            }

            if(found)
            {
                if(sendHeadersData.headers.size() == 0)
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending zero headers", sendHeadersData.headers.size());
                else
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending %d headers starting at height %d",
                      sendHeadersData.headers.size(),
                      mChain->hashHeight(sendHeadersData.headers.front().hash()));
                if(sendMessage(&sendHeadersData))
                    mStatistics.headersSent += sendHeadersData.headers.size();
            }
            else
            {
                NextCash::Hash emptyHash;
                if(mChain->getHeaders(sendHeadersData.headers, emptyHash,
                  getHeadersData->stopHeaderHash, 2000))
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending %d headers starting at genesis",
                      sendHeadersData.headers.size());
                    if(sendMessage(&sendHeadersData))
                        mStatistics.headersSent += sendHeadersData.headers.size();
                }
            }
            break;
        }
        case Message::INVENTORY:
            if(isOutgoing())
            {
                Message::InventoryData *inventoryData = (Message::InventoryData *)message;
                unsigned int blockCount = 0;
                bool headersNeeded = false;
                NextCash::HashList blockList, transactionList;
                Message::NotFoundData notFound;

                if(mMemPoolRequested)
                    mMemPoolReceived = true;

                for(Message::Inventory::iterator item = inventoryData->inventory.begin();
                  item != inventoryData->inventory.end() && !mStopRequested; ++item)
                {
                    switch((*item)->type)
                    {
                    case Message::InventoryHash::BLOCK:
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Block Inventory : %s", (*item)->hash.hex().text());
                        blockCount++;
                        addAnnouncedBlock((*item)->hash);

                        switch(mChain->addPendingHash((*item)->hash, mID))
                        {
                        case Chain::HEADER_NEEDED:
                            headersNeeded = true;
                            mLastBlockAnnounced = (*item)->hash;
                            break;
                        case Chain::BLOCK_NEEDED:
                            blockList.push_back((*item)->hash);
                            break;
                        case Chain::INVALID:
                            sendReject(Message::nameFor(message->type),
                              Message::RejectData::WRONG_CHAIN,
                              "Announced block failed verification");
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Dropping. Black listed block announced : %s",
                              (*item)->hash.hex().text());
                            close();
                            success = false;
                            break;
                        case Chain::ALREADY_HAVE:
                        default:
                            break;
                        }
                        break;
                    case Message::InventoryHash::TRANSACTION:
                        // NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                          // "Transaction announced : %s", (*item)->hash.hex().text());

                        if(addAnnouncedTransaction((*item)->hash) && info.spvMode)
                            transactionList.push_back((*item)->hash);

                        if(!info.spvMode)
                        {
                            switch(mChain->memPool().hashStatus((*item)->hash, mID,
                              false))
                            {
                            case MemPool::HASH_NEED:
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction needed : %s", (*item)->hash.hex().text());
                                transactionList.push_back((*item)->hash);
                                break;
                            case MemPool::HASH_REQUESTED:
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction already requested : %s",
                                  // (*item)->hash.hex().text());
                                mSavedTransactions.push_back((*item)->hash);
                                break;
                            case MemPool::HASH_PROCESSING:
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction processing : %s",
                                  // (*item)->hash.hex().text());
                                break;
                            case MemPool::HASH_ALREADY_HAVE:
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction already have : %s",
                                  // (*item)->hash.hex().text());
                                break;
                            case MemPool::HASH_INVALID:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::INVALID, "Failed verification",
                                  (*item)->hash);
                                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                                  "Dropping. Invalid transaction announced : %s",
                                  (*item)->hash.hex().text());
                                info.addPeerFail(mAddress);
                                close();
                                success = false;
                                break;
                            case MemPool::HASH_LOW_FEE:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::LOW_FEE, "Low Fee", (*item)->hash);
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction low fee : %s", (*item)->hash.hex().text());
                                break;
                            case MemPool::HASH_NON_STANDARD:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::NON_STANDARD, "Non Standard",
                                  (*item)->hash);
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction non standard : %s",
                                  // (*item)->hash.hex().text());
                                break;
                            case MemPool::HASH_DOUBLE_SPEND:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::DUPLICATE, "Double Spend",
                                  (*item)->hash);
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction is double spend : %s",
                                  // (*item)->hash.hex().text());
                                break;
                            case MemPool::HASH_REJECTED_ANCESTOR:
                                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                                  // "Transaction is rejected ancestor : %s",
                                  // (*item)->hash.hex().text());
                                break;
                            }
                        }
                        break;
                    case Message::InventoryHash::FILTERED_BLOCK: // Should never be in an inventory message
                    case Message::InventoryHash::COMPACT_BLOCK: // Should never be in an inventory message
                        break;
                    default:
                        NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                          "Unknown Transaction Inventory Type : %02x", (*item)->type);
                        break;
                    }

                    if(!isOpen())
                        break;
                }

                if(blockCount > 1)
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Received %d block inventory", blockCount);

                if(headersNeeded)
                    requestHeaders();

                if(!info.spvMode && blockList.size() > 0 && !waitingForBlockRequests() &&
                  !waitingForHeaderRequests())
                    requestBlocks(blockList);

                if(transactionList.size() > 0)
                    requestTransactions(transactionList, false);

                if(notFound.inventory.size() > 0)
                    sendMessage(&notFound);
            }
            break;
        case Message::HEADERS:
            if(isIncoming())
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Incoming node sent %d headers",
                  ((Message::HeadersData *)message)->headers.size());
                info.addPeerFail(mAddress, 5);
                close();
                success = false;
            }
            else if(mReceivedVersionData != NULL)
            {
                Message::HeadersData *headersData = (Message::HeadersData *)message;
                unsigned int addedCount = 0;
                NextCash::HashList hashList;
                bool lastAnnouncedHeaderFound = mLastBlockAnnounced.isEmpty() ||
                  mChain->headerAvailable(mLastBlockAnnounced);
                Chain::HashStatus headerStatus;

                if(headersData->headers.size() == 0)
                {
                    addAnnouncedBlock(mHeaderRequested);
                    if(info.chainID != CHAIN_UNKNOWN)
                    {
                        mChainID = info.chainID;
                        info.markPeerChain(mAddress, mChainID);
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Provided no headers. Assuming matching chain %s",
                          chainName(mChainID));
                    }
                    requestPeers();
                }
                else
                    addAnnouncedBlock(headersData->headers.back().hash());

                if(headersData->headers.size() == 1)
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received header : %s", headersData->headers.front().hash().hex().text());
                else
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received %d headers", headersData->headers.size());

                mHeaderRequested.clear();
                mHeaderRequestTime = 0;
                mStatistics.headersReceived += headersData->headers.size();
                bool shortChain = false, invalidHeader = false, otherChain = false;
                NextCash::Hash shortHash, lastHash;

                for(HeaderList::iterator header = headersData->headers.begin();
                  header != headersData->headers.end() && !mStopRequested && !invalidHeader;
                  ++header)
                {
                    if(!mLastBlockAnnounced.isEmpty() && mLastBlockAnnounced == header->hash())
                        lastAnnouncedHeaderFound = true;

                    headerStatus = mChain->addHeader(*header);
                    lastHash = header->hash();
                    switch(headerStatus)
                    {
                    case Chain::ALREADY_HAVE:
                        shortChain = false;
                        break;
                    case Chain::HEADER_NEEDED:
                        shortChain = false;
                        break;
                    case Chain::HEADER_ADDED:
                        ++addedCount;
                        shortChain = false;
                        break;
                    case Chain::BLOCK_NEEDED:
                        shortChain = false;
                        ++addedCount;
                        if(!info.spvMode && mChain->isInSync())
                            hashList.push_back(header->hash());
                        break;
                    case Chain::BLOCK_ADDED:
                        shortChain = false;
                        break;
                    case Chain::SHORT_CHAIN:
                        if(isOutgoing())
                        {
                            shortHash = header->hash();
                            shortChain = true;
                        }
                        break;
                    case Chain::INVALID:
                    case Chain::UNKNOWN:
                        invalidHeader = true;
                        break;
                    }
                }

                if(!lastHash.isEmpty() &&
                  info.chainID != CHAIN_UNKNOWN && mChainID == CHAIN_UNKNOWN)
                {
                    switch(headerStatus)
                    {
                    case Chain::ALREADY_HAVE:
                    case Chain::HEADER_NEEDED:
                    case Chain::HEADER_ADDED:
                    case Chain::BLOCK_NEEDED:
                    case Chain::BLOCK_ADDED:
                    {
                        unsigned int headerHeight = mChain->hashHeight(lastHash);
                        if(chainSplitHeight(info.chainID) < headerHeight)
                        {
                            mChainID = info.chainID;
                            info.markPeerChain(mAddress, mChainID);
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Provided header from %s chain : %s",
                              chainName(mChainID), lastHash.hex().text());
                        }

                        // Header is fairly recent
                        if(mChain->headerHeight() < headerHeight + 6000)
                            requestPeers();
                        break;
                    }
                    case Chain::SHORT_CHAIN:
                    case Chain::INVALID:
                    case Chain::UNKNOWN:
                        if(BTC_SPLIT_HASH == lastHash)
                        {
                            otherChain = true;
                            mChainID = CHAIN_BTC;
                            info.markPeerChain(mAddress, mChainID);
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Dropping. Provided header from %s chain : %s",
                              chainName(mChainID), lastHash.hex().text());
                            close();
                        }
                        else if(ABC_SPLIT_HASH == lastHash)
                        {
                            otherChain = true;
                            mChainID = CHAIN_ABC;
                            info.markPeerChain(mAddress, mChainID);
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Dropping. Provided header from %s chain : %s",
                              chainName(mChainID), lastHash.hex().text());
                            close();
                        }
                        else if(SV_SPLIT_HASH == lastHash)
                        {
                            otherChain = true;
                            mChainID = CHAIN_SV;
                            info.markPeerChain(mAddress, mChainID);
                            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                              "Dropping. Provided header from %s chain : %s",
                              chainName(mChainID), lastHash.hex().text());
                            close();
                        }
                        break;
                    }
                }

                if(shortChain)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Provided header from short chain : %s",
                      shortHash.hex().text());
                    close();
                    success = false;
                    break;
                }

                if(success)
                {
                    if(hashList.size() > 0)
                    {
                        if(!info.spvMode && !waitingForBlockRequests() &&
                          !waitingForHeaderRequests() && compactBlocksEnabled())
                            requestBlocks(hashList);
                    }
                    else if(!lastAnnouncedHeaderFound && mChain->isInSync())
                    {
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Announced header hash for which they didn't provide header : %s",
                          mLastBlockAnnounced.hex().text());
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }

                    if(addedCount > 1)
                    {
                        info.addPeerSuccess(mAddress, 1);
                        mChain->setHeadersNeeded(); // Immediately request more headers.
                    }
                    else if(invalidHeader && !otherChain)
                    {
                        NextCash::Log::add(NextCash::Log::INFO, mName,
                          "Dropping. Outgoing node sent bad header");
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }

                    mLastBlockAnnounced.clear();

                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Added %d pending headers", addedCount);

                    if(success && !isScan() && info.chainID == CHAIN_UNKNOWN)
                        requestPeers();
                }

                if(isScan())
                    close();
            }
            break;
        case Message::BLOCK:
            if(isOutgoing())
            {
                if(info.spvMode)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Sent block in SPV mode : %s",
                      ((Message::BlockData *)message)->block->header.hash().hex().text());
                    info.addPeerFail(mAddress, 5);
                    close();
                    success = false;
                }
                else
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received block (%d) (%d KB) : %s",
                      mChain->hashHeight(((Message::BlockData *)message)->block->header.hash()),
                      ((Message::BlockData *)message)->block->size() / 1000,
                      ((Message::BlockData *)message)->block->header.hash().hex().text());
                    ++mStatistics.blocksReceived;

                    // unsigned int offset = 0;
                    // for(std::vector<Transaction *>::iterator trans =
                      // ((Message::BlockData *)message)->block->transactions.begin();
                      // trans != ((Message::BlockData *)message)->block->transactions.end();
                      // ++trans, ++offset)
                        // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          // "Transaction %d : %s", offset, (*trans)->hash().hex().text());

                    updateBlockRequest(((Message::BlockData *)message)->block->header.hash(),
                      message, true);

                    // Remove from any pending compact block.
                    for(std::vector<Message::CompactBlockData *>::iterator block =
                      mIncomingCompactBlocks.begin(); block != mIncomingCompactBlocks.end();
                      ++block)
                        if((*block)->block->header.hash() ==
                          ((Message::BlockData *)message)->block->header.hash())
                        {
                            delete *block;
                            mIncomingCompactBlocks.erase(block);
                            break;
                        }

                    time = getTime();
                    if(mMessageInterpreter.pendingBlockStartTime != 0 &&
                      time - mMessageInterpreter.pendingBlockStartTime > 60)
                    {
                        // Drop after the block finishes so it doesn't have to be restarted
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Block download took %ds",
                          time - mMessageInterpreter.pendingBlockStartTime);
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }

                    if(mChain->addBlock(((Message::BlockData *)message)->block) ==
                      Chain::BLOCK_ADDED)
                    {
                        // Memory has been handed off
                        ((Message::BlockData *)message)->block = NULL;
                        if(!isSeed() && mReceivedVersionData != NULL)
                            info.addPeerSuccess(mAddress, 1);
                    }
                }
            }
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Incoming node sent block : %s",
                  ((Message::BlockData *)message)->block->header.hash().hex().text());
                info.addPeerFail(mAddress, 5);
                close();
                success = false;
            }
            break;
        case Message::TRANSACTION:
        {
            if(!info.spvMode)
                --mMessagesReceived; // Don't count towards message limit

            // Verify and add to mem pool or monitor.
            Message::TransactionData *transactionData = (Message::TransactionData *)message;
            if(transactionData->transaction)
            {
                // NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  // "Received transaction (%d bytes) : %s",
                  // transactionData->transaction->size(),
                  // transactionData->transaction->hash.hex().text());

                mTransactionsRequested.remove(transactionData->transaction->hash());

                if(!info.spvMode)
                {
                    NextCash::HashList unseen, allUnseen;
                    mChain->memPool().add(transactionData->transaction);
                }
                else if(mMonitor != NULL)
                    mMonitor->addTransaction(*mChain, transactionData->transaction);
            }
            break;
        }
        case Message::MEM_POOL:
            if(!info.spvMode)
            {
                // Send Inventory message with all transactions in the mem pool
                Message::InventoryData inventoryMessage;
                NextCash::HashList list;

                mChain->memPool().getFullList(list, mFilter);

                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Sending %d mempool transaction hashes", list.size());

                for(NextCash::HashList::iterator hash = list.begin(); hash != list.end();
                  ++hash)
                {
                    if(inventoryMessage.inventory.size() == 10000)
                    {
                        // For large mem pools break in to multiple messages
                        if(!sendMessage(&inventoryMessage))
                            break;
                        inventoryMessage.inventory.clear();
                    }

                    inventoryMessage.inventory.push_back(
                      new Message::InventoryHash(Message::InventoryHash::TRANSACTION, *hash));
                }

                if(inventoryMessage.inventory.size() > 0)
                    sendMessage(&inventoryMessage);
            }
            break;
        case Message::MERKLE_BLOCK:
            mLastMerkleReceive = getTime();
            --mActiveMerkleRequests;
            --mMessagesReceived; // Don't count to reduce turnover when syncing
            if(isOutgoing() && mMonitor != NULL &&
              !mMonitor->addMerkleBlock(*mChain, (Message::MerkleBlockData *)message, mID) &&
              !mChain->headerAvailable(((Message::MerkleBlockData *)message)->header.hash()))
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Invalid Merkle Block : %s",
                  ((Message::MerkleBlockData *)message)->header.hash().hex().text());
                close();
                success = false;
            }
            break;
        case Message::NOT_FOUND:
        {
            Message::NotFoundData *notFoundData = (Message::NotFoundData *)message;
            for(Message::Inventory::iterator item = notFoundData->inventory.begin();
              success && item != notFoundData->inventory.end(); ++item)
            {
                switch((*item)->type)
                {
                case Message::InventoryHash::BLOCK:
                {
                    bool wasRequested = false;
                    mBlockRequestMutex.lock();
                    for(NextCash::HashList::iterator hash = mBlocksRequested.begin();
                      hash != mBlocksRequested.end(); ++hash)
                        if(*hash == (*item)->hash)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Block hash returned not found : %s", hash->hex().text());
                            wasRequested = true;
                            break;
                        }
                    mBlockRequestMutex.unlock();

                    if(wasRequested)
                    {
                        NextCash::Log::add(NextCash::Log::INFO, mName,
                          "Dropping. Blocks not found");
                        close();
                        success = false;
                    }
                    break;
                }
                case Message::InventoryHash::TRANSACTION:
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Transaction hash returned not found : %s", (*item)->hash.hex().text());
                    if(mChain->memPool().release((*item)->hash, mID))
                    {
                        NextCash::Log::add(NextCash::Log::INFO, mName,
                          "Dropping. Failed to provide outpoint for given transaction");
                        info.addPeerFail(mAddress);
                        close();
                        success = false;
                    }
                    break;
                case Message::InventoryHash::FILTERED_BLOCK:
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Merkle block hash returned not found : %s",
                      (*item)->hash.hex().text());
                    close();
                    success = false;
                    break;
                case Message::InventoryHash::COMPACT_BLOCK:
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Compact block hash returned not found : %s",
                      (*item)->hash.hex().text());
                    break;
                case Message::InventoryHash::UNKNOWN:
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Unknown \"not found\" inventory item type %d : %s", (*item)->type,
                      (*item)->hash.hex().text());
                    break;
                }
            }
            break;
        }
        case Message::SEND_COMPACT:
        {
            Message::SendCompactData *sendCompactData = (Message::SendCompactData *)message;
            if(sendCompactData->version == 1L)
            {
                mSendCompactBlocksVersion = sendCompactData->version;
                if(sendCompactData->sendCompact == 1)
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Compact block (announcing) version 0x%08x%08x",
                      sendCompactData->version >> 32, sendCompactData->version & 0xffffffff);
                    mAnnounceBlocksCompact = true;
                }
                else if(sendCompactData->sendCompact == 0)
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Compact block version 0x%08x%08x",
                      sendCompactData->version >> 32, sendCompactData->version & 0xffffffff);
                    mAnnounceBlocksCompact = false;
                }
            }
            else
            {
                sendReject(Message::nameFor(message->type), Message::RejectData::DECODE,
                  "Unsupported compact block version");
                NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
                  "Compact block unsupported version 0x%08x%08x",
                  sendCompactData->version >> 32, sendCompactData->version & 0xffffffff);
            }

            if(!info.spvMode && !mSendCompactSent)
            {
                // Send compact block configuration.
                Message::SendCompactData sendCompactBlocks(mRequestAnnounceCompact, 1L);
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Sending compact version 0x%08x%08x", sendCompactBlocks.version >> 32,
                  sendCompactBlocks.version & 0xffffffff);
                sendMessage(&sendCompactBlocks);
                mSendCompactSent = true;
            }
            break;
        }
        case Message::COMPACT_BLOCK:
            if(isOutgoing())
            {
                if(info.spvMode)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Sent compact block in SPV mode : %s",
                      ((Message::CompactBlockData *)message)->block->header.hash().hex().text());
                    info.addPeerFail(mAddress, 5);
                    close();
                    success = false;
                }
                else
                {
                    Message::CompactBlockData *compactBlockData =
                      (Message::CompactBlockData *)message;

                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received compact block (%d trans) (%d KB) : %s",
                      compactBlockData->shortIDs.size() + compactBlockData->prefilled.size(),
                      compactBlockData->size / 1000L,
                      compactBlockData->block->header.hash().hex().text());

                    if(mAnnounceBlocksCompact)
                        addAnnouncedBlock(compactBlockData->block->header.hash());

                    Chain::HashStatus headerStatus = mChain->addHeader(
                      compactBlockData->block->header,
                      mID);
                    if(headerStatus == Chain::INVALID || headerStatus == Chain::UNKNOWN)
                    {
                        NextCash::Log::add(NextCash::Log::INFO, mName,
                          "Dropping. Sent compact block with invalid header");
                        info.addPeerFail(mAddress, 1);
                        mChain->releaseBlockForNode(compactBlockData->block->header.hash(),
                          mID);
                        close();
                        success = false;
                        break;
                    }
                    else if(headerStatus != Chain::BLOCK_NEEDED)
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Already have compact block (%d) : %s",
                          mChain->hashHeight(compactBlockData->block->header.hash()),
                          compactBlockData->block->header.hash().hex().text());
                        break;
                    }

                    bool marked = mChain->markBlockForNode(
                      compactBlockData->block->header.hash(), mID);

                    unsigned int height = mChain->hashHeight(
                      compactBlockData->block->header.hash());
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Compact block height %d : %s", height,
                      compactBlockData->block->header.hash().hex().text());

                    if(mMessageInterpreter.pendingBlockHash ==
                       compactBlockData->block->header.hash() &&
                       mMessageInterpreter.pendingBlockStartTime != 0 &&
                       time - mMessageInterpreter.pendingBlockStartTime > 60)
                    {
                        // Drop after the block finishes so it doesn't have to be restarted
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Compact block download took %ds",
                          time - mMessageInterpreter.pendingBlockStartTime);
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                        break;
                    }

                    mProcessingCompactTransactions = true;
                    FillResult fillResult = fillCompactBlock(compactBlockData, marked);
                    if(fillResult == FILL_COMPLETE) // Block full
                    {
                        updateBlockRequest(compactBlockData->block->header.hash(), message,
                          true);
                        if(mChain->addBlock(compactBlockData->block) == Chain::BLOCK_ADDED)
                            compactBlockData->block = NULL;
                    }
                    else if(fillResult == FILL_INCOMPLETE) // Wait for transactions
                    {
                        updateBlockRequest(compactBlockData->block->header.hash(), message,
                          false);
                        mIncomingCompactBlocks.push_back(compactBlockData);
                        dontDeleteMessage = true;
                    }
                    else if(marked) // FILL_FAILED
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Requesting full block : %s",
                          compactBlockData->block->header.hash().hex().text());
                        NextCash::HashList list;
                        list.push_back(compactBlockData->block->header.hash());
                        requestBlocks(list, true);
                    }
                    compactBlockData->time = getTime(); // Prevent timeout
                    mMessageInterpreter.pendingBlockUpdateTime = getTime();
                    mProcessingCompactTransactions = false;
                }
            }
            break;
        case Message::GET_COMPACT_TRANS:
            if(info.spvMode)
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Sent get compact trans in SPV mode : %s",
                  ((Message::GetCompactTransData *)message)->headerHash.hex().text());
                info.addPeerFail(mAddress, 5);
                close();
                success = false;
            }
            else
            {
                Message::GetCompactTransData *getCompactTransData =
                  (Message::GetCompactTransData *)message;
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Received get compact block transactions : %s",
                  getCompactTransData->headerHash.hex().text());

                for(std::vector<Message::CompactBlockData *>::iterator compact =
                  mOutgoingCompactBlocks.begin(); compact != mOutgoingCompactBlocks.end();)
                    if((*compact)->block->header.hash() == getCompactTransData->headerHash)
                    {
                        // Send CompactTransData with missing transactions.
                        Message::CompactTransData transData(
                          getCompactTransData->headerHash);

                        unsigned int offset = 0;
                        for(std::vector<unsigned int>::iterator encodedOffset =
                          getCompactTransData->offsets.begin();
                            encodedOffset !=
                            getCompactTransData->offsets.end(); ++encodedOffset)
                        {
                            offset += *encodedOffset;
                            if(offset >= (*compact)->block->transactions.size())
                            {
                                NextCash::Log::add(NextCash::Log::WARNING, mName,
                                  "Dropping. Get compact block transaction offset past end of block");
                                info.addPeerFail(mAddress, 5);
                                close();
                                success = false;
                                break;
                            }
                            transData.transactions
                              .push_back((*compact)->block->transactions.at(offset));
                        }

                        if(success)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Sending %d compact block transactions : %s",
                              transData.transactions.size(),
                              getCompactTransData->headerHash.hex().text());
                            sendMessage(&transData);
                        }

                        transData.transactions.clear(); // Prevent from being delete twice.
                        delete *compact;
                        mOutgoingCompactBlocks.erase(compact);
                        break;
                    }
            }
            break;
        case Message::COMPACT_TRANS:
            if(isOutgoing())
            {
                if(info.spvMode)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Sent compact trans in SPV mode : %s",
                      ((Message::CompactTransData *)message)->headerHash.hex().text());
                    info.addPeerFail(mAddress, 5);
                    close();
                    success = false;
                }
                else
                {
                    Message::CompactTransData *compactTransData =
                      (Message::CompactTransData *)message;
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received %d compact block transactions",
                      compactTransData->transactions.size());

                    if(mMessageInterpreter.pendingBlockHash == compactTransData->headerHash &&
                       mMessageInterpreter.pendingBlockStartTime != 0 &&
                       time - mMessageInterpreter.pendingBlockStartTime > 60)
                    {
                        // Drop after the block finishes so it doesn't have to be restarted
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Compact block transaction download took %ds",
                          time - mMessageInterpreter.pendingBlockStartTime);
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }

                    bool found = false;
                    for(std::vector<Message::CompactBlockData *>::iterator block =
                      mIncomingCompactBlocks.begin();
                        block != mIncomingCompactBlocks.end(); ++block)
                        if(compactTransData->headerHash == (*block)->block->header.hash())
                        {
                            found = true;
                            mProcessingCompactTransactions = true;
                            if(addTransactionsToCompactBlock(*block, compactTransData))
                            {
                                if(mChain->addBlock((*block)->block) == Chain::BLOCK_ADDED)
                                    (*block)->block = NULL;
                            }
                            else
                                mChain->releaseBlockForNode(compactTransData->headerHash, mID);
                            updateBlockRequest(compactTransData->headerHash, message, true);
                            delete *block;
                            mIncomingCompactBlocks.erase(block);
                            mProcessingCompactTransactions = false;
                            break;
                        }

                    if(!found)
                        NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                          "Received compact block transactions for unknown block");
                }
            }
            break;

        case Message::GET_GRAPHENE:
        case Message::GRAPHENE:
        case Message::GET_GRAPHENE_TRANS:
        case Message::GRAPHENE_TRANS:
        case Message::UNKNOWN:
            break;
        }

        if(!dontDeleteMessage)
            delete message;

        return success;
    }
}
