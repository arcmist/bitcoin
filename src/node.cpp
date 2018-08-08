/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "node.hpp"

#include "digest.hpp"
#include "info.hpp"
#include "message.hpp"
#include "block.hpp"
#include "chain.hpp"
#include "interpreter.hpp"


#define PEER_MESSAGE_LIMIT 5000
#define PEER_TIME_LIMIT 1800


namespace BitCoin
{
    unsigned int Node::mNextID = 256;

    Node::Node(NextCash::Network::Connection *pConnection, Chain *pChain, bool pIncoming,
      bool pIsSeed, bool pIsGood, uint64_t pServices, Monitor &pMonitor) : mID(mNextID++),
      mConnectionMutex("Node Connection"), mBlockRequestMutex("Node Block Request"),
      mAnnounceMutex("Node Announce")
    {
        mIsIncoming = pIncoming;
        mConnected = false;
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
        mBlockReceiveTime = 0;
        mLastReceiveTime = getTime();
        mLastCheckTime = getTime();
        mLastBlackListCheck = getTime();
        mLastPingNonce = 0;
        mLastPingTime = 0;
        mPingRoundTripTime = -1;
        mPingCutoff = 30;
        mBlockDownloadCount = 0;
        mBlockDownloadSize = 0;
        mBlockDownloadTime = 0;
        mMessagesReceived = 0;
        mPingCount = 0;
        mConnectedTime = getTime();
        mStarted = false;
        mStopRequested = false;
        mStopped = false;
        mIsSeed = pIsSeed;
        mIsGood = pIsGood;
        mSendBlocksCompact = false;
        mRejected = false;
        mWasReady = false;
        mReleased = false;
#ifndef SINGLE_THREAD
        mThread = NULL;
#endif
        mSocketID = -1;
        mServices = pServices;
        if(!pIncoming && !pIsSeed)
            mMonitor = &pMonitor;
        else
            mMonitor = NULL;
        mActiveMerkleRequests = 0;
        mLastMerkleCheck = 0;
        mLastMerkleRequest = 0;
        mBloomFilterID = 0;
        mChain = pChain;

        if(mIsIncoming)
            mName.writeFormatted("Node i[%d]", mID);
        else
            mName.writeFormatted("Node o[%d]", mID);

        // Verify connection
        mConnectionMutex.lock();
        mConnection = pConnection;
        mSocketID = pConnection->socket();
        if(!mIsIncoming)
            mAddress = *mConnection;
        if(!mConnection->isOpen())
        {
            mConnectionMutex.unlock();
            mStopped = true;
            if(!mIsSeed && !mIsIncoming)
                Info::instance().addPeerFail(mAddress, 1, 1);
            return;
        }
        mConnected = true;
        mConnectionMutex.unlock();
        if(mIsIncoming)
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Incoming Connection %s : %d (socket %d)", mConnection->ipv6Address(),
              mConnection->port(), mSocketID);
        else
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Outgoing Connection %s : %d (socket %d)", mConnection->ipv6Address(),
              mConnection->port(), mSocketID);

#ifndef SINGLE_THREAD
        // Start thread
        mThread = new NextCash::Thread(mName, run, this);
        NextCash::Thread::sleep(500); // Give the thread a chance to initialize
#endif
    }

    Node::~Node()
    {
        int timeout = 25;
        while(!mStarted && --timeout)
            NextCash::Thread::sleep(200); // Wait for thread initialize before destroying

        if(mConnected)
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName, "Disconnecting (socket %d)", mSocketID);
        if(!mMessageInterpreter.pendingBlockHash.isEmpty())
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Dropped block in progress %d KiB (%d secs) : %s", mReceiveBuffer.length() / 1024,
              mMessageInterpreter.pendingBlockUpdateTime - mMessageInterpreter.pendingBlockStartTime,
              mMessageInterpreter.pendingBlockHash.hex().text());

        requestStop();
        release();
#ifndef SINGLE_THREAD
        if(mThread != NULL)
            delete mThread;
#endif
        mConnectionMutex.lock();
        if(mConnection != NULL)
            delete mConnection;
        mConnectionMutex.unlock();
        if(mSentVersionData != NULL)
            delete mSentVersionData;
        if(mReceivedVersionData != NULL)
            delete mReceivedVersionData;
    }

    bool Node::isOpen()
    {
        mConnectionMutex.lock();
        bool result = mConnection != NULL && mConnection->isOpen();
        mConnectionMutex.unlock();
        return result;
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

        if(mMonitor != NULL)
            mMonitor->release(mID);
        mBlockRequestMutex.lock();
        if(mBlocksRequested.size() > 0 || !mHeaderRequested.isEmpty())
            mChain->releaseBlocksForNode(mID);
        mBlocksRequested.clear();
        mHeaderRequested.clear();
        mBlockRequestMutex.unlock();
        mChain->memPool().releaseForNode(mID);
        mReleased = true;
    }

    void Node::close()
    {
        mConnectionMutex.lock();
        if(mConnection != NULL)
            mConnection->close();
        mConnectionMutex.unlock();
        requestStop();
    }

    void Node::requestStop()
    {
#ifndef SINGLE_THREAD
        if(mThread == NULL)
            return;
#endif
        mStopRequested = true;
        release();
    }

    void Node::collectStatistics(Statistics &pCollection)
    {
        mConnectionMutex.lock();
        mStatistics.bytesReceived += mConnection->bytesReceived();
        mStatistics.bytesSent += mConnection->bytesSent();
        mConnection->resetByteCounts();
        mConnectionMutex.unlock();
        pCollection += mStatistics;
        mStatistics.clear();
    }

    void Node::prepare()
    {
        if(mLastPingTime == 0)
            sendPing();

        if(!mPrepared && isReady())
        {
            Info &info = Info::instance();

            if(info.spvMode)
                sendBloomFilter();
            else if(!mIsIncoming)
                sendFeeFilter();

            if(!mIsIncoming && !mIsSeed)
            {
                if(mSentVersionData != NULL && (mSentVersionData->relay || mBloomFilterID != 0))
                {
                    Message::Data memPoolMessage(Message::MEM_POOL);
                    sendMessage(&memPoolMessage);
                }

                Message::Data sendHeadersMessage(Message::SEND_HEADERS);
                sendMessage(&sendHeadersMessage);
            }

            if(mIsGood)
                requestPeers();
            requestHeaders();

            if(mReceivedVersionData != NULL && !mIsIncoming && !mIsSeed)
                info.updatePeer(mAddress, mReceivedVersionData->userAgent,
                  mReceivedVersionData->transmittingServices);

            mPrepared = true;
        }
    }

    bool Node::check()
    {
        int32_t time = getTime();
        mLastCheckTime = time;

        if(!isOpen())
            return false;

        if(mIsSeed && time - mConnectedTime > 120)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName,
              "Dropping. Seed connected for too long.");
            close();
            return false;
        }

        if(mPingRoundTripTime == -1 && time - mConnectedTime > mPingCutoff)
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Dropping. Not ready within %d seconds of connection.", mPingCutoff);
            Info::instance().addPeerFail(mAddress, 5);
            close();
            return false;
        }

        if(!mIsIncoming)
        {
            if(mBlocksRequested.size() > 0 && time - mBlockRequestTime > 30 &&
              time - mBlockReceiveTime > 30)
            {
                // Haven't started receiving blocks 30 seconds after requesting
                if(mMessageInterpreter.pendingBlockUpdateTime == 0)
                {
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. No block for 30 seconds");
                    Info::instance().addPeerFail(mAddress);
                    close();
                    return false;
                }

                // Haven't received more of the block in the last 30 seconds
                if(time - mMessageInterpreter.pendingBlockUpdateTime > 30)
                {
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. No update on block for 30 seconds");
                    Info::instance().addPeerFail(mAddress);
                    close();
                    return false;
                }
            }

            if(!mHeaderRequested.isEmpty() && time - mHeaderRequestTime > 30)
            {
                NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Not providing headers");
                Info::instance().addPeerFail(mAddress);
                close();
                return false;
            }

            if(mLastReceiveTime != 0 && time - mLastReceiveTime > 1200)
            {
                NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Not responding");
                Info::instance().addPeerFail(mAddress);
                close();
                return false;
            }
        }

        return true;
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

    bool Node::waitingForRequests()
    {
        if(mBlocksRequested.size() > 0)
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Waiting for %d blocks",
              mBlocksRequested.size());
            return true;
        }
        else if(!mHeaderRequested.isEmpty())
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
              "Waiting for headers after : %s", mHeaderRequested.hex().text());
            return true;
        }
        else
            return false;
    }

    bool Node::requestHeaders()
    {
        if(mStopRequested || !isOpen() || !isReady() || mIsIncoming || waitingForRequests())
            return false;

        if(!mLastHeaderRequested.isEmpty() &&
          mLastHeaderRequested == mChain->lastPendingBlockHash())
            return false;

        if(!mLastHeader.isEmpty() && mLastHeader == mChain->lastPendingBlockHash())
            return false;

        NextCash::HashList hashes;
        if(!mChain->getReverseBlockHashes(hashes, 16))
            return false;

        Message::GetHeadersData getHeadersData;
        for(NextCash::HashList::iterator hash=hashes.begin();hash!=hashes.end();++hash)
            getHeadersData.blockHeaderHashes.push_back(*hash);

        if(hashes.size() == 0)
            NextCash::Log::add(NextCash::Log::VERBOSE, mName,
              "Sending request for block headers from genesis");
        else
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending request for block headers after %d : %s", mChain->height(),
              hashes.front().hex().text());
        bool success = sendMessage(&getHeadersData);
        if(success)
        {
            mHeaderRequested = hashes.front();
            mLastHeaderRequested = hashes.front();
            mHeaderRequestTime = getTime();
        }
        return success;
    }

    bool Node::requestBlocks(NextCash::HashList &pList)
    {
        if(pList.size() == 0 || mStopRequested || !isOpen() || mIsIncoming || mIsSeed)
            return false;

        // Put block hashes into block request message
        Message::GetDataData getDataData;
        for(NextCash::HashList::iterator hash = pList.begin(); hash != pList.end(); ++hash)
            getDataData.inventory
              .push_back(new Message::InventoryHash(Message::InventoryHash::BLOCK, *hash));

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
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Sending request for block at (%d) : %s", mChain->blockHeight(pList.front()),
                  pList.front().hex().text());
            else
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Sending request for %d blocks starting at (%d) : %s", pList.size(),
                  mChain->blockHeight(pList.front()), pList.front().hex().text());
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

    bool Node::hasTransaction(const NextCash::Hash &pHash)
    {
        mAnnounceMutex.lock();
        bool result = mAnnounceTransactions.contains(pHash);
        mAnnounceMutex.unlock();
        return result;
    }

    bool Node::requestTransactions(NextCash::HashList &pList)
    {
        if(pList.size() == 0 || !isOpen() || mIsIncoming || mIsSeed)
            return false;

        // Put transaction hashes into transaction request message
        Message::GetDataData message;
        for(NextCash::HashList::iterator hash=pList.begin();hash!=pList.end();++hash)
            message.inventory
              .push_back(new Message::InventoryHash(Message::InventoryHash::TRANSACTION, *hash));

        bool success = sendMessage(&message);
        if(success)
        {
            mChain->memPool().markForNode(pList, mID);
            if(pList.size() == 1)
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  "Sending request for transaction %s", pList.front().hex().text());
            else
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  "Sending request for %d transactions starting with %s", pList.size(),
                  pList.front().hex().text());
        }
        else
            mChain->memPool().releaseForNode(mID);

        return success;
    }

    bool Node::requestPeers()
    {
        NextCash::Log::add(NextCash::Log::INFO, mName, "Sending peer request");
        Message::Data getAddresses(Message::GET_ADDRESSES);
        return sendMessage(&getAddresses);
    }

    bool Node::sendBlock(Block &pBlock)
    {
        if(!isOpen())
            return false;

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Sending block : %s",
          pBlock.hash.hex().text());
        Message::BlockData blockData;
        blockData.block = &pBlock;
        bool success = sendMessage(&blockData);
        if(success)
            ++mStatistics.blocksSent;
        blockData.block = NULL; // We don't want to delete the block when the message is deleted
        return success;
    }

    bool Node::sendMerkleBlock(const NextCash::Hash &pBlockHash)
    {
        Block block;

        if(!mChain->getBlock(pBlockHash, block))
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Merkle block not found : %s", pBlockHash.hex().text());
            return false;
        }

        std::vector<Transaction *> includedTransactions;
        Message::MerkleBlockData merkleMessage(&block, mFilter, includedTransactions);
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
          "Sending merkle block with %d trans : %s", includedTransactions.size(),
          pBlockHash.hex().text());
        if(!sendMessage(&merkleMessage))
            return false;

        Message::TransactionData transactionMessage;
        for(std::vector<Transaction *>::iterator trans = includedTransactions.begin();
          trans != includedTransactions.end(); ++trans)
        {
            transactionMessage.transaction = *trans;
            if(!sendMessage(&transactionMessage))
            {
                transactionMessage.transaction = NULL; // Prevent from being double deleted
                return false;
            }
        }
        transactionMessage.transaction = NULL; // Prevent from being double deleted

        return true;
    }

    bool Node::announceBlock(Block *pBlock)
    {
        if(!isOpen() || mReceivedVersionData == NULL)
            return false;

        mAnnounceMutex.lock();
        if(mAnnounceBlocks.contains(pBlock->hash))
        {
            // Don't announce to node that already announced to you
            mAnnounceMutex.unlock();
            return false;
        }
        mAnnounceMutex.unlock();

        // if(mReceivedVersionData->transmittingServices & Message::VersionData::XTHIN_NODE_BIT)

        //TODO if(mSendBlocksCompact)
        // {
            // //TODO  Send CompactBlockData
            // //NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
            // //  "Announcing block with compact : %s", pBlock->hash.hex().text());
            // return false;
        // }
        // else

        if(mSendHeaders)
        {
            // Send the header
            Message::HeadersData headersData;
            headersData.headers.push_back(pBlock);
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Announcing block with header : %s", pBlock->hash.hex().text());
            bool success = sendMessage(&headersData);
            if(success)
                mStatistics.headersSent += headersData.headers.size();
            // We don't want to delete pBlock since it will be reused
            headersData.headers.clearNoDelete();
            return success;
        }
        else
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Announcing block with hash : %s", pBlock->hash.hex().text());
            Message::InventoryData inventoryData;
            inventoryData.inventory.
              push_back(new Message::InventoryHash(Message::InventoryHash::BLOCK, pBlock->hash));
            return sendMessage(&inventoryData);
        }
    }

    bool Node::announceTransaction(Transaction *pTransaction)
    {
        if(!isOpen() || mReceivedVersionData == NULL)
            return false;

        bool filterContains = mFilter.contains(*pTransaction);

        if(!mReceivedVersionData->relay && !filterContains)
            return false;

        if(filterContains)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Bloom filter contains transaction : %s", pTransaction->hash.hex().text());

            // Update filter
            if(mFilter.flags() & BloomFilter::UPDATE_MASK)
            {
                ScriptInterpreter::ScriptType type;
                NextCash::HashList hashes;
                Outpoint outpoint;

                outpoint.transactionID = pTransaction->hash;
                outpoint.index = 0;

                for(std::vector<Output>::iterator output = pTransaction->outputs.begin();
                  output != pTransaction->outputs.end(); ++output, ++outpoint.index)
                    if(mFilter.containsScript(output->script))
                    {
                        if(mFilter.flags() & BloomFilter::UPDATE_P2PUBKEY_ONLY)
                        {
                            // Don't add unless P2PKH or MultiSig
                            type = ScriptInterpreter::parseOutputScript(output->script, hashes);
                            if(type != ScriptInterpreter::P2PKH &&
                              type != ScriptInterpreter::MULTI_SIG)
                                continue;
                        }

                        // Add new UTXO to filter
                        mFilter.add(outpoint);
                    }
            }
        }
        else // Full relay mode
        {
            mAnnounceMutex.lock();
            if(mAnnounceTransactions.contains(pTransaction->hash))
            {
                // Don't announce to node that already announced to you
                mAnnounceMutex.unlock();
                return false;
            }
            mAnnounceMutex.unlock();

            // Check against minimum fee rate
            if(pTransaction->feeRate() < mMinimumFeeRate)
            {
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  "Not announcing transaction fee rate %d below min rate %d : %s",
                  pTransaction->feeRate(), mMinimumFeeRate, pTransaction->hash.hex().text());
                return false;
            }
        }

        // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
          // "Announcing transaction with fee rate %d above min rate %d : %s", pTransaction->feeRate(),
          // mMinimumFeeRate, pTransaction->hash.hex().text());
        Message::InventoryData inventoryData;
        inventoryData.inventory
          .push_back(new Message::InventoryHash(Message::InventoryHash::TRANSACTION,
          pTransaction->hash));
        return sendMessage(&inventoryData);
    }

    bool Node::sendTransaction(Transaction *pTransaction)
    {
        if(!isOpen() || mReceivedVersionData == NULL || mSentTransactions.contains(pTransaction->hash))
            return false;

        bool filterContains = mFilter.contains(*pTransaction);

        if(!mReceivedVersionData->relay && !filterContains)
            return false;

        Message::TransactionData transactionData;
        transactionData.transaction = pTransaction;
        bool result = sendMessage(&transactionData);
        transactionData.transaction = NULL;

        if(result)
        {
            mSentTransactions.push_back(pTransaction->hash);
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Sent transaction : %s", pTransaction->hash.hex().text());
        }
        else
            NextCash::Log::addFormatted(NextCash::Log::WARNING, mName,
              "Failed to send transaction : %s", pTransaction->hash.hex().text());

        return result;
    }

    bool Node::sendVersion()
    {
        if(!isOpen())
            return false;

        Info &info = Info::instance();
        if(mSentVersionData != NULL)
            delete mSentVersionData;
        mSentVersionData = new Message::VersionData(mConnection->ipv6Bytes(), mConnection->port(),
          mServices, info.ip, info.port, info.spvMode, mChain->height(),
          (!mIsIncoming && !mIsSeed && info.initialBlockDownloadIsComplete()));
        bool success = sendMessage(mSentVersionData);
        mVersionSent = true;
        return success;
    }

    bool Node::sendPing()
    {
        int32_t time = getTime();
        if(time - mLastPingTime < 60)
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
        Message::FeeFilterData feeData;
        feeData.minimumFeeRate = Info::instance().minFee;
        return sendMessage(&feeData);
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
                                  const char *pReason,
      const NextCash::Hash &pHash)
    {
        if(!isOpen())
            return false;

        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Sending reject : %s", pReason);
        Message::RejectData rejectMessage(pCommand, pCode, pReason, NULL);
        pHash.write(&rejectMessage.extra);
        return sendMessage(&rejectMessage);
    }

    void Node::run()
    {
        Node *node = (Node *)NextCash::Thread::getParameter();
        if(node == NULL)
        {
            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_NODE_LOG_NAME,
              "Thread parameter is null. Stopping");
            return;
        }
        node->mStarted = true;

        NextCash::String name = node->mName;

        if(node->mStopRequested)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, name,
              "Node stopped before thread started");
            node->mStopped = true;
            return;
        }

        while(!node->mStopRequested)
        {
            node->process();

            if(node->mStopRequested)
                break;

            NextCash::Thread::sleep(100);
        }

        node->mStopped = true;
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
        }
        mAnnounceMutex.unlock();
    }

    bool Node::addAnnouncedTransaction(const NextCash::Hash &pHash)
    {
        mAnnounceMutex.lock();
        if(!mAnnounceTransactions.contains(pHash))
        {
            // Keep list at 1024 or less
            if(mAnnounceTransactions.size() > 1024)
                mAnnounceTransactions.erase(mAnnounceTransactions.begin());
            mAnnounceTransactions.push_back(pHash);
        }
        mAnnounceMutex.unlock();

        return mMonitor != NULL && mMonitor->addTransactionAnnouncement(pHash, mID);
    }

    void Node::process()
    {
        if(!isOpen() || mStopRequested || mStopped)
            return;

        if(!mVersionSent)
            sendVersion();

        if(mMessagesReceived > PEER_MESSAGE_LIMIT)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Reached message limit");
            close();
            return;
        }

        int32_t time = getTime();
        if(time - mConnectedTime > PEER_TIME_LIMIT)
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Reached time limit");
            close();
            return;
        }

        if(time - mLastCheckTime > 5 && !check())
            return; // Closed

        mConnectionMutex.lock();
        mReceiveBuffer.compact();

        if(mConnection == NULL)
        {
            mConnectionMutex.unlock();
            return;
        }

        if(!mConnection->isOpen())
        {
            mConnectionMutex.unlock();
            return;
        }

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

        if(info.spvMode && isReady() && !mIsIncoming && !mIsSeed && mMonitor != NULL &&
          time - mLastMerkleCheck > 2)
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

            if(mActiveMerkleRequests < 25)
            {
                bool fail = false;
                NextCash::HashList blockHashes;

                mMonitor->getNeededMerkleBlocks(mID, *mChain, blockHashes);
                for(NextCash::HashList::iterator hash = blockHashes.begin();
                  hash != blockHashes.end(); ++hash)
                    if(!requestMerkleBlock(*hash))
                    {
                        fail = true;
                        break;
                    }

                if(!fail && blockHashes.size() > 0)
                {
                    mActiveMerkleRequests += blockHashes.size();
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Requested %d merkle blocks", blockHashes.size());
                    mLastMerkleRequest = getTime();
                }

                mLastMerkleCheck = time;
            }
            else if(time - mLastMerkleCheck > 120)
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Dropping. Took too long to return merkle blocks");
                close();
                return;
            }
        }

        if(mReceivedVersionData != NULL && mVersionAcknowledged && mLastPingTime != 0 &&
           mPingRoundTripTime == -1 && mPingCutoff != -1 &&
           time - mLastPingTime > mPingCutoff)
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

    bool Node::processMessage()
    {
        if(mMessagesReceived > PEER_MESSAGE_LIMIT && !mStopRequested)
            return false;

        // Check for a complete message
        Message::Data *message;
        bool dontDeleteMessage = false;
        bool success = true;
        int32_t time = getTime();
        Info &info = Info::instance();

        if(mMessagesReceived == 0 && failedStartBytes())
        {
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
            if(!mIsSeed)
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
                      "More than one version message");
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

                std::memcpy(mAddress.ip, mReceivedVersionData->transmittingIPv6, 16);
                mAddress.port = mReceivedVersionData->transmittingPort;
                mMessageInterpreter.version = mReceivedVersionData->version;

                if(!mAddress.isValid() || mAddress.port == 0)
                {
                    std::memcpy(mAddress.ip, mConnection->ipv6Address(), 16);
                    mAddress.port = mConnection->port();
                }

                // Require full node bit for outgoing nodes
                if(!mIsIncoming && !mIsSeed && !(mReceivedVersionData->transmittingServices &
                  Message::VersionData::FULL_NODE_BIT))
                {
                    sendReject(Message::nameFor(message->type), Message::RejectData::PROTOCOL,
                      "Full node bit (0x01) required in protocol version");
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. Missing full node bit");
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                }
                else if(!mIsIncoming && !mIsSeed && !mChain->isInSync() &&
                  (mReceivedVersionData->startBlockHeight < 0 ||
                  mReceivedVersionData->startBlockHeight < mChain->height()))
                {
                    NextCash::Log::add(NextCash::Log::INFO, mName, "Dropping. Low block height");
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                }
                else if(info.spvMode && !mIsSeed && !(mReceivedVersionData->transmittingServices &
                  Message::VersionData::BLOOM_NODE_BIT))
                {
                    mRejected = true;
                    sendReject(Message::nameFor(message->type), Message::RejectData::PROTOCOL,
                      "Bloom node bit (0x04) required in protocol version");
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. Missing bloom node bit");
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                }
                else
                {
                    // Send version acknowledge
                    Message::Data versionAcknowledgeMessage(Message::VERACK);
                    sendMessage(&versionAcknowledgeMessage);
                    mVersionAcknowledgeSent = true;

                    if(mIsSeed)
                        requestPeers(); // Request addresses from the
                    else if(mVersionAcknowledged)
                        prepare();
                }
                break;
            }
            case Message::VERACK:
                mVersionAcknowledged = true;
                if(mReceivedVersionData != NULL && !mIsSeed)
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
                    if(mPingRoundTripTime == -1)
                    {
                        NextCash::Log::add(NextCash::Log::DEBUG, mName, "Received round trip ping");
                        mPingRoundTripTime = time - mLastPingTime;
                        if(!mIsIncoming && !mIsSeed && mPingCutoff != -1)
                        {
                            if(mPingRoundTripTime > mPingCutoff)
                            {
                                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                                  "Dropping. Ping time %ds not within cutoff of %ds",
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
                info.getRandomizedPeers(peers, 1, servicesMask);

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
            {
                Message::AddressesData *addressesData = (Message::AddressesData *)message;
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Received %d peer addresses", addressesData->addresses.size());
                IPAddress ip;

                for(std::vector<Message::Address>::iterator address =
                  addressesData->addresses.begin(); address != addressesData->addresses.end() &&
                  !mStopRequested; ++address)
                {
                    ip.set(address->ip, address->port);
                    info.addPeer(ip, address->services);
                }

                if(mIsSeed)
                {
                    NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                      "Closing seed because it gave addresses");
                    close(); // Disconnect from seed node because it has done its job
                }
                break;
            }
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
                for(std::vector<NextCash::Hash>::iterator i =
                  getBlocksData->blockHeaderHashes.begin();
                  i != getBlocksData->blockHeaderHashes.end(); ++i)
                    if(mChain->getBlockHashes(hashes, *i, 500))
                        break;

                if(hashes.size() == 0)
                {
                    // No matching starting hashes found. Start from genesis
                    NextCash::Hash emptyHash;
                    mChain->getBlockHashes(hashes, emptyHash, 500);
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
                Block block;
                bool fail = false;

                for(Message::Inventory::iterator item = getDataData->inventory.begin();
                  item != getDataData->inventory.end() && !mStopRequested; ++item)
                {
                    switch((*item)->type)
                    {
                    case Message::InventoryHash::BLOCK:
                    {
                        int height = mChain->blockHeight((*item)->hash);

                        if(height == -1)
                            notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                        else if(height < mReceivedVersionData->startBlockHeight - 1000)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Not sending block. Block height %d below node's start block height %d : %s",
                              height, mReceivedVersionData->startBlockHeight, (*item)->hash.hex().text());
                        }
                        else if(mChain->getBlock((*item)->hash, block))
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
                        Message::TransactionData transactionData;
                        transactionData.transaction = mChain->memPool().get((*item)->hash);
                        if(transactionData.transaction == NULL)
                            notFoundData.inventory.push_back(new Message::InventoryHash(**item));
                        else
                        {
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Sending Transaction (%d bytes) : %s",
                              transactionData.transaction->size(), (*item)->hash.hex().text());
                            sendMessage(&transactionData);
                        }
                        // Don't delete it. It is still in the mem pool
                        transactionData.transaction = NULL;
                        break;
                    }
                    case Message::InventoryHash::FILTERED_BLOCK:
                        sendMerkleBlock((*item)->hash);
                        break;
                    case Message::InventoryHash::COMPACT_BLOCK:
                        //TODO Implement GET_DATA compact blocks (COMPACT_BLOCK)
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Requested Compact Block (Not implemented) : %s",
                          (*item)->hash.hex().text());
                        break;
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
                int height;
                bool found = false;

                for(std::vector<NextCash::Hash>::iterator hash =
                  getHeadersData->blockHeaderHashes.begin();
                  hash != getHeadersData->blockHeaderHashes.end(); ++hash)
                {
                    height = mChain->blockHeight(*hash);
                    if(height != -1)
                    {
                        if(height > 5000 && height < mReceivedVersionData->startBlockHeight - 5000)
                        {
                            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                              "Not sending headers. Header height %d below node's start block height %d : %s",
                              height, mReceivedVersionData->startBlockHeight, hash->hex().text());
                            break;
                        }
                        else if(mChain->getBlockHeaders(sendHeadersData.headers, *hash,
                          getHeadersData->stopHeaderHash, 2000))
                        {
                            found = true;
                            break; // match found
                        }
                    }
                }

                if(found)
                {
                    if(sendHeadersData.headers.size() == 0)
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Sending zero block headers", sendHeadersData.headers.size());
                    else
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Sending %d block headers starting at height %d",
                          sendHeadersData.headers.size(),
                          mChain->blockHeight(sendHeadersData.headers.front()->hash));
                    if(sendMessage(&sendHeadersData))
                        mStatistics.headersSent += sendHeadersData.headers.size();
                }
                break;
            }
            case Message::INVENTORY:
                if(!mIsIncoming && !mIsSeed)
                {
                    Message::InventoryData *inventoryData = (Message::InventoryData *)message;
                    unsigned int blockCount = 0;
                    bool headersNeeded = false;
                    NextCash::HashList blockList, transactionList;
                    Message::NotFoundData notFound;

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

                            // Clear last header request so it doesn't prevent a new header request
                            mLastHeaderRequested.clear();

                            switch(mChain->addPendingHash((*item)->hash, mID))
                            {
                                case Chain::NEED_HEADER:
                                    headersNeeded = true;
                                    mLastBlockAnnounced = (*item)->hash;
                                    break;
                                case Chain::NEED_BLOCK:
                                    blockList.push_back((*item)->hash);
                                    break;
                                case Chain::BLACK_LISTED:
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
                                    break;
                            }
                            break;
                        case Message::InventoryHash::TRANSACTION:
                            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                              "Transaction Inventory : %s", (*item)->hash.hex().text());

                            if(addAnnouncedTransaction((*item)->hash) && info.spvMode)
                                transactionList.push_back((*item)->hash);

                            if(!info.spvMode)
                            {
                                switch(mChain->memPool().addPending((*item)->hash, mChain, mID))
                                {
                                    case MemPool::NEED:
                                        transactionList.push_back((*item)->hash);
                                        break;
                                    case MemPool::ALREADY_HAVE:
                                        break;
                                    case MemPool::BLACK_LISTED:
                                        sendReject(Message::nameFor(message->type), Message::RejectData::WRONG_CHAIN,
                                          "Announced transaction failed verification");
                                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                                          "Dropping. Black listed transaction announced : %s", (*item)->hash.hex().text());
                                        close();
                                        success = false;
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

                    if(blockList.size() > 0)
                        requestBlocks(blockList);

                    if(transactionList.size() > 0)
                        requestTransactions(transactionList);

                    if(notFound.inventory.size() > 0)
                        sendMessage(&notFound);
                }
                break;
            case Message::HEADERS:
                if(!mIsIncoming && !mIsSeed)
                {
                    Message::HeadersData *headersData = (Message::HeadersData *)message;
                    unsigned int addedCount = 0;
                    NextCash::HashList blockList;
                    bool lastAnnouncedHeaderFound = mLastBlockAnnounced.isEmpty() ||
                      mChain->headerAvailable(mLastBlockAnnounced);
                    bool unsolicited = mHeaderRequested.isEmpty();
                    int status, badHeadersCount = 0;

                    if(headersData->headers.size() == 0)
                        mLastHeader = mHeaderRequested;
                    else
                        mLastHeader = headersData->headers.back()->hash;

                    // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      // "Last header set : %s", mLastHeader.hex().text());

                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Received %d block headers", headersData->headers.size());
                    mHeaderRequested.clear();
                    mHeaderRequestTime = 0;
                    mStatistics.headersReceived += headersData->headers.size();

                    for(std::vector<Block *>::iterator header = headersData->headers.begin();
                      header != headersData->headers.end() && !mStopRequested;)
                    {
                        if(!mLastBlockAnnounced.isEmpty() && mLastBlockAnnounced == (*header)->hash)
                            lastAnnouncedHeaderFound = true;

                        status = mChain->addPendingBlock(*header);
                        if(status == 0)
                        {
                            // Memory will be deleted by block chain after it is processed so
                            //   remove it from this list
                            header = headersData->headers.erase(header);
                            addedCount++;

                            if(!info.spvMode && mChain->isInSync())
                                blockList.push_back((*header)->hash);

                            if(unsolicited)
                                mChain->setAnnouncedAdded();
                        }
                        else
                        {
                            if(status == -1)
                                ++badHeadersCount;
                            ++header;
                        }
                    }

                    if(blockList.size() > 0)
                        requestBlocks(blockList);
                    else if(!lastAnnouncedHeaderFound && mChain->isInSync())
                    {
                        mRejected = true;
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Announced block for which they didn't provide header : %s",
                          mLastBlockAnnounced.hex().text());
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }

                    if(!mIsSeed && mReceivedVersionData != NULL)
                    {
                        if(addedCount > 0)
                        {
                            info.addPeerSuccess(mAddress, 1);

                            // Immediately request more headers if in SPV mode so all the headers
                            //   come back to back
                            if(info.spvMode && addedCount > 1000)
                                requestHeaders();
                        }
                        else if(badHeadersCount >= 500)
                        {
                            if(!mIsIncoming)
                            {
                                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                                  "Dropping. Outgoing node sent %d bad headers",
                                  ((Message::HeadersData *)message)->headers.size());
                                info.addPeerFail(mAddress, 5);
                                close();
                                success = false;
                            }
                        }
                    }

                    mLastBlockAnnounced.clear();

                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Added %d pending headers", addedCount);
                }
                else
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Incoming node sent %d headers",
                      ((Message::HeadersData *)message)->headers.size());
                    info.addPeerFail(mAddress, 5);
                    close();
                    success = false;
                }
                break;
            case Message::BLOCK:
                if(!mIsIncoming && !mIsSeed)
                {
                    if(info.spvMode)
                    {
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Dropping. Sent block in SPV mode : %s",
                          ((Message::BlockData *)message)->block->hash.hex().text());
                        info.addPeerFail(mAddress, 5);
                        close();
                        success = false;
                    }
                    else
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Received block (height %d) (%d KiB) : %s",
                          mChain->blockHeight(((Message::BlockData *)message)->block->hash),
                          ((Message::BlockData *)message)->block->size() / 1024,
                          ((Message::BlockData *)message)->block->hash.hex().text());
                        ++mStatistics.blocksReceived;

                        // Remove from blocks requested
                        time = getTime();
                        mBlockRequestMutex.lock();
                        for(NextCash::HashList::iterator hash = mBlocksRequested.begin();
                          hash != mBlocksRequested.end(); ++hash)
                            if(*hash == ((Message::BlockData *)message)->block->hash)
                            {
                                mBlocksRequested.erase(hash);
                                mBlockReceiveTime = time;
                                ++mBlockDownloadCount;
                                mBlockDownloadTime +=
                                  time - mMessageInterpreter.pendingBlockStartTime;
                                mBlockDownloadSize +=
                                  ((Message::BlockData *)message)->block->size();
                                break;
                            }
                        mBlockRequestMutex.unlock();

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

                        if(mChain->addPendingBlock(((Message::BlockData *)message)->block) == 0)
                        {
                            // Memory has been handed off
                            ((Message::BlockData *)message)->block = NULL;
                            if(!mIsSeed && mReceivedVersionData != NULL)
                                info.addPeerSuccess(mAddress, 1);
                        }
                    }
                }
                else
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Incoming node sent block : %s",
                      ((Message::BlockData *)message)->block->hash.hex().text());
                    info.addPeerFail(mAddress, 5);
                    close();
                    success = false;
                }
                break;
            case Message::TRANSACTION:
            {
                if(mBloomFilterID == 0 && (mSentVersionData == NULL || mSentVersionData->relay == 0x00))
                {
                    NextCash::Log::add(NextCash::Log::INFO, mName,
                      "Dropping. Received transaction when relay is off and no bloom filter was sent");
                    info.addPeerFail(mAddress);
                    close();
                    success = false;
                    break;
                }

                // Verify and add to mem pool
                Message::TransactionData *transactionData = (Message::TransactionData *)message;
                if(transactionData->transaction != NULL)
                {
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                      "Received transaction (%d bytes) : %s", transactionData->transaction->size(),
                      transactionData->transaction->hash.hex().text());

                    if(!info.spvMode)
                    {
                        MemPool::AddStatus addStatus =
                          mChain->memPool().add(transactionData->transaction, info.minFee, mChain);

                        switch(addStatus)
                        {
                            case MemPool::ADDED:
                            case MemPool::UNSEEN_OUTPOINTS: // Added to pending
                                if(mMonitor != NULL)
                                {
                                    transactionData->transaction =
                                      new Transaction(*transactionData->transaction);
                                    mMonitor->addTransaction(*mChain, transactionData);
                                }
                                else // So it won't be deleted with the message
                                    transactionData->transaction = NULL;
                                break;

                            case MemPool::NON_STANDARD:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::NON_STANDARD, "Non standard",
                                  transactionData->transaction->hash);
                                break;

                            case MemPool::DOUBLE_SPEND:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::DUPLICATE, "Double spend",
                                  transactionData->transaction->hash);
                                break;

                            case MemPool::LOW_FEE:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::LOW_FEE, "Fee below minimum",
                                  transactionData->transaction->hash);
                                break;

                            case MemPool::INVALID:
                                sendRejectWithHash(Message::nameFor(message->type),
                                  Message::RejectData::INVALID, "Invalid transaction",
                                  transactionData->transaction->hash);

                                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                                  "Dropping. Sent invalid transaction : %s",
                                  transactionData->transaction->hash.hex().text());
                                info.addPeerFail(mAddress);
                                close();
                                success = false;
                                break;

                            default:
                                break;
                        }

                    }
                    else if(mMonitor != NULL)
                        mMonitor->addTransaction(*mChain, transactionData);
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
                      "Sending %d mem pool transaction hashes", list.size());

                    for(NextCash::HashList::iterator hash=list.begin();hash!=list.end();++hash)
                    {
                        if(inventoryMessage.inventory.size() == 10000)
                        {
                            // For large mem pools break in to multiple messages
                            if(!sendMessage(&inventoryMessage))
                                break;
                            inventoryMessage.inventory.clear();
                        }

                        inventoryMessage.inventory
                          .push_back(new Message::InventoryHash(Message::InventoryHash::TRANSACTION,
                          *hash));
                    }

                    if(inventoryMessage.inventory.size() > 0)
                        sendMessage(&inventoryMessage);
                }
                break;
            case Message::MERKLE_BLOCK:
                --mActiveMerkleRequests;
                --mMessagesReceived; // Don't count to reduce turnover when syncing
                if(!mIsIncoming && !mIsSeed && mMonitor != NULL &&
                  !mMonitor->addMerkleBlock(*mChain, (Message::MerkleBlockData *)message, mID) &&
                  !mChain->blockInChain(((Message::MerkleBlockData *)message)->block->hash))
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Dropping. Invalid Merkle Block : %s",
                      ((Message::MerkleBlockData *)message)->block->hash.hex().text());
                    close();
                    success = false;
                }
                break;
            case Message::NOT_FOUND:
            {
                Message::NotFoundData *notFoundData = (Message::NotFoundData *)message;
                for(Message::Inventory::iterator item = notFoundData->inventory.begin();
                  item != notFoundData->inventory.end(); ++item)
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
                          "Compact block hash returned not found : %s", (*item)->hash.hex().text());
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

                if(sendCompactData->encoding == 1)
                {
                    if(sendCompactData->sendCompact == 1)
                    {
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Send Compact Activated");
                        mSendBlocksCompact = true;
                    }
                    else if(sendCompactData->sendCompact == 0)
                        mSendBlocksCompact = false;
                }
                else
                {
                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Unknown Send Compact encoding %08x%08x", sendCompactData->encoding >> 32,
                      sendCompactData->encoding & 0xffffffff);
                }
                break;
            }
            case Message::COMPACT_BLOCK:
            {
                //TODO Message::CompactBlockData *compactBlockData = (Message::CompactBlockData *)message;
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "Compact block (Not implemented)");
                break;
            }
            case Message::GET_BLOCK_TRANSACTIONS:
            {
                //TODO Message::GetBlockTransactionsData *getBlockTransactionsData = (Message::GetBlockTransactionsData *)message;
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "Get compact block transactions (Not implemented)");
                break;
            }
            case Message::BLOCK_TRANSACTIONS:
            {
                //TODO Message::BlockTransactionsData *blockTransactionsData = (Message::BlockTransactionsData *)message;
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "Compact block transactions (Not implemented)");
                break;
            }

            case Message::UNKNOWN:
                break;
        }

        if(!dontDeleteMessage)
            delete message;

        return success;
    }
}
