/**************************************************************************
 * Copyright 2017 ArcMist, LLC                                            *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@arcmist.com>                                    *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "chain.hpp"

#ifdef PROFILER_ON
#include "arcmist/dev/profiler.hpp"
#endif

#include "arcmist/base/log.hpp"
#include "arcmist/base/thread.hpp"
#include "arcmist/io/file_stream.hpp"
#include "arcmist/crypto/digest.hpp"
#include "info.hpp"
#include "daemon.hpp"

#define BITCOIN_CHAIN_LOG_NAME "BitCoin Chain"


namespace BitCoin
{
    Chain::Chain() : mPendingLock("Chain Pending"),
      mProcessMutex("Chain Process")
    {
        mMaxTargetBits = 0x1d00ffff;
        mNextBlockHeight = 0;
        mLastFileID = 0;
        mPendingSize = 0;
        mPendingBlocks = 0;
        mTargetBits = 0;
        mLastTargetTime = 0;
        mLastBlockTime = 0;
        mLastBlockFile = NULL;
        mLastFullPendingOffset = 0;
    }

    Chain::~Chain()
    {
        mPendingLock.writeLock("Destroy");
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            delete *pending;
        mPendingLock.writeUnlock();
    }

    bool Chain::revertTargetBits(unsigned int pHeight)
    {
        mTargetBits = mBlockStats.targetBits(pHeight);

        mLastTargetBits = mBlockStats.targetBits(pHeight - 1);
        mLastBlockTime = mBlockStats.time(pHeight - 1);

        unsigned int lastRetargetHeight = pHeight - (pHeight % RETARGET_PERIOD);
        mLastTargetTime = mBlockStats.time(lastRetargetHeight);

        return saveTargetBits();
    }

    bool Chain::updateTargetBits(unsigned int pHeight, uint32_t pNextBlockTime, uint32_t pNextBlockTargetBits)
    {
        if(mLastTargetTime == 0)
        {
            // This is the first block
            mTargetBits = mMaxTargetBits;
            mLastTargetTime = pNextBlockTime;
            mLastBlockTime = pNextBlockTime;
            mLastTargetBits = pNextBlockTargetBits;
            return saveTargetBits();
        }
        else if(pHeight == 0 || pHeight % RETARGET_PERIOD != 0)
        {
            mLastBlockTime = pNextBlockTime;
            mLastTargetBits = pNextBlockTargetBits;
            return true;
        }

        // Calculate percent of time actually taken for the last 2016 blocks by the goal time of 2 weeks
        // Adjust factor over 1.0 means the target is going up, which also means the difficulty to
        //   find a hash under the target goes down
        // Adjust factor below 1.0 means the target is going down, which also means the difficulty to
        //   find a hash under the target goes up
        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
          "Time spent on last 2016 blocks %d - %d = %d", mLastBlockTime, mLastTargetTime, mLastBlockTime - mLastTargetTime);
        double adjustFactor = (double)(mLastBlockTime - mLastTargetTime) / 1209600.0;

        if(adjustFactor > 1.0)
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Increasing target bits %08x by a factor of %f to reduce difficulty by %.02f%%", mLastTargetBits,
              adjustFactor, (1.0 - (1.0 / adjustFactor)) * 100.0);
        else
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Decreasing target bits %08x by a factor of %f to increase difficulty by %.02f%%", mLastTargetBits,
              adjustFactor, ((1.0 / adjustFactor) - 1.0) * 100.0);

        if(adjustFactor < 0.25)
        {
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Changing target adjust factor to 0.25 because of maximum decrease of 75%");
            adjustFactor = 0.25; // Maximum decrease of 75%
        }
        else if(adjustFactor > 4.0)
        {
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Changing target adjust factor to 4.0 because of maximum increase of 400%");
            adjustFactor = 4.0; // Maximum increase of 400%
        }

        /* Note: an off-by-one error in the Bitcoin Core implementation causes the difficulty to be
         * updated every 2,016 blocks using timestamps from only 2,015 blocks, creating a slight skew.
         */

        // Treat targetValue as a 256 bit number and multiply it by adjustFactor
        mTargetBits = multiplyTargetBits(mLastTargetBits, adjustFactor, mMaxTargetBits);
        mLastTargetTime = pNextBlockTime;
        mLastBlockTime = pNextBlockTime;
        mLastTargetBits = pNextBlockTargetBits;

        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
          "New target bits for block height %d : %08x", pHeight, mTargetBits);
        return saveTargetBits();
    }

    bool Chain::saveTargetBits()
    {
        // Save to a file
        ArcMist::String filePathName = Info::instance().path();
        filePathName.pathAppend("blocks");
        filePathName.pathAppend("target");
        ArcMist::FileOutputStream *file = new ArcMist::FileOutputStream(filePathName, true);
        file->setOutputEndian(ArcMist::Endian::LITTLE);
        if(file->isValid())
        {
            file->writeUnsignedInt(mLastTargetTime);
            file->writeUnsignedInt(mTargetBits);
        }
        else
        {
            delete file;
            return false;
        }
        delete file;
        return true;
    }

    bool Chain::loadTargetBits()
    {
        if(mNextBlockHeight == 0)
        {
            mLastBlockTime = 0;
            mLastTargetTime = 0;
            mTargetBits = 0;
            return true;
        }

        // Get last block time
        Block block;
        if(!getBlock(mNextBlockHeight - 1, block))
        {
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Failed to read last block from file");
            return false;
        }
        mLastBlockTime = block.time;
        mLastTargetBits = block.targetBits;

        bool success = true;
        ArcMist::String filePathName = Info::instance().path();
        filePathName.pathAppend("blocks");
        filePathName.pathAppend("target");
        ArcMist::FileInputStream *file = new ArcMist::FileInputStream(filePathName);
        file->setInputEndian(ArcMist::Endian::LITTLE);
        if(file->isValid())
        {
            mLastTargetTime = file->readUnsignedInt();
            mTargetBits = file->readUnsignedInt();
            ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
              "Loaded target bits of %08x", mTargetBits);
        }
        else
        {
            //TODO Recalculate
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Failed to read target bits file");
            success = false;
        }
        delete file;
        return success;
    }

    bool Chain::headerAvailable(Hash &pHash)
    {
        if(blockInChain(pHash))
            return true;

        bool found = false;
        mPendingLock.readLock();
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            if((*pending)->block->hash == pHash)
            {
                found = true;
                break;
            }
        mPendingLock.readUnlock();
        return found;
    }

    unsigned int Chain::blockFileID(const Hash &pHash)
    {
        if(pHash.isEmpty())
            return 0; // Empty hash means start from the beginning

        uint16_t lookup = pHash.lookup16();
        unsigned int result = INVALID_FILE_ID;

        mBlockLookup[lookup].lock();
        BlockSet::iterator end = mBlockLookup[lookup].end();
        for(BlockSet::iterator i=mBlockLookup[lookup].begin();i!=end;++i)
            if(pHash == (*i)->hash)
            {
                result = (*i)->fileID;
                mBlockLookup[lookup].unlock();
                return result;
            }
        mBlockLookup[lookup].unlock();
        return result;
    }

    int Chain::height(const Hash &pHash)
    {
        int result = -1;
        if(pHash.isEmpty())
            return result; // Empty hash means start from the beginning

        uint16_t lookup = pHash.lookup16();
        mBlockLookup[lookup].lock();
        BlockSet::iterator end = mBlockLookup[lookup].end();
        for(BlockSet::iterator i=mBlockLookup[lookup].begin();i!=end;++i)
            if(pHash == (*i)->hash)
            {
                result = (*i)->height;
                break;
            }
        mBlockLookup[lookup].unlock();

        if(result == -1)
        {
            // Check pending
            int currentHeight = blockHeight();
            mPendingLock.readLock();
            for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            {
                ++currentHeight;
                if((*pending)->block->hash == pHash)
                {
                    result = currentHeight;
                    break;
                }
            }
            mPendingLock.readUnlock();
        }

        return result;
    }

    unsigned int Chain::pendingCount()
    {
        mPendingLock.readLock();
        unsigned int result = mPending.size();
        mPendingLock.readUnlock();
        return result;
    }

    unsigned int Chain::pendingBlockCount()
    {
        mPendingLock.readLock();
        unsigned int result = mPendingBlocks;
        mPendingLock.readUnlock();
        return result;
    }

    unsigned int Chain::pendingSize()
    {
        mPendingLock.readLock();
        unsigned int result = mPendingSize;
        mPendingLock.readUnlock();
        return result;
    }

    // Add block header to queue to be requested and downloaded
    bool Chain::addPendingHeader(Block *pBlock)
    {
        bool result = false;
        mPendingLock.writeLock("Add");
        if(mPending.size() == 0)
        {
            if(pBlock->previousHash.isZero() && mLastBlockHash.isEmpty())
                result = true; // First block of chain
            else if(pBlock->previousHash == mLastBlockHash)
                result = true; // First pending entry
        }
        else if(mPending.back()->block->hash == pBlock->previousHash)
            result = true; // Add to pending

        if(!result)
        {
            mPendingLock.writeUnlock();
            if(blockInChain(pBlock->hash) || headerAvailable(pBlock->hash))
                ArcMist::Log::addFormatted(ArcMist::Log::DEBUG, BITCOIN_CHAIN_LOG_NAME,
                  "Header already downloaded : %s", pBlock->hash.hex().text());
            else
                ArcMist::Log::addFormatted(ArcMist::Log::DEBUG, BITCOIN_CHAIN_LOG_NAME,
                  "Unknown header : %s", pBlock->hash.hex().text());
            return false;
        }

        // This just checks that the proof of work meets the target bits in the header.
        //   The validity of the target bits value is checked before adding the full block to the chain.
        if(!pBlock->hasProofOfWork())
        {
            mPendingLock.writeUnlock();
            ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
              "Not enough proof of work : %s", pBlock->hash.hex().text());
            Hash target;
            target.setDifficulty(pBlock->targetBits);
            ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
              "Target                   : %s", target.hex().text());
            return false;
        }

        // Add to pending list
        mPending.push_back(new PendingData(pBlock));
        mLastPendingHash = pBlock->hash;
        mPendingSize += pBlock->size();

        //TODO if(!result) check if this header is from an alternate chain.
        //  Check if previous hash is in the chain, but not at the top and determine if a fork is needed

        mPendingLock.writeUnlock();

        if(result)
            ArcMist::Log::addFormatted(ArcMist::Log::DEBUG, BITCOIN_CHAIN_LOG_NAME,
              "Added pending header : %s", pBlock->hash.hex().text());
        return result;
    }

    bool Chain::savePending()
    {
        mPendingLock.readLock();
        if(mPending.size() == 0)
        {
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "No pending blocks/headers to save to the file system");
            mPendingLock.readUnlock();
            return false;
        }

        ArcMist::String filePathName = Info::instance().path();
        filePathName.pathAppend("pending");
        ArcMist::FileOutputStream file(filePathName, true);

        if(!file.isValid())
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed to open file to save pending blocks/headers to the file system");
            mPendingLock.readUnlock();
            return false;
        }

        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            (*pending)->block->write(&file, true, true);

        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
          "Saved %d/%d pending blocks/headers to the file system",
          mPendingBlocks, mPending.size() - mPendingBlocks);

        mPendingLock.readUnlock();
        return true;
    }

    bool Chain::loadPending()
    {
        ArcMist::String filePathName = Info::instance().path();
        filePathName.pathAppend("pending");
        if(!ArcMist::fileExists(filePathName))
        {
            ArcMist::Log::add(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
              "No file to load pending blocks/headers from the file system");
            return true;
        }

        ArcMist::FileInputStream file(filePathName);
        if(!file.isValid())
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed to open file to load pending blocks/headers from the file system");
            return false;
        }

        bool success = true;
        Block *newBlock;

        mPendingLock.writeLock("Load");

        // Clear pending (just in case)
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            delete *pending;
        mPending.clear();
        mPendingSize = 0;
        mPendingBlocks = 0;
        unsigned int offset = 0;

        // Read pending blocks/headers from file
        while(file.remaining())
        {
            newBlock = new Block();
            if(!newBlock->read(&file, true, true, true))
            {
                delete newBlock;
                success = false;
                break;
            }
            mPendingSize += newBlock->size();
            if(newBlock->transactionCount > 0)
                mPendingBlocks++;
            mPending.push_back(new PendingData(newBlock));
            if(mPending.back()->isFull())
                mLastFullPendingOffset = offset;
            ++offset;
        }

        if(success)
        {
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Loaded %d/%d pending blocks/headers from the file system",
              mPendingBlocks, mPending.size() - mPendingBlocks);
            mLastPendingHash = mPending.back()->block->hash;
        }
        else
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed to load pending blocks/headers from the file system");
            // Clear all pending that was read because it may be invalid
            for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
                delete *pending;
            mPending.clear();
            mPendingSize = 0;
            mPendingBlocks = 0;
            mLastFullPendingOffset = 0;
        }

        mPendingLock.writeUnlock();
        file.close();
        ArcMist::removeFile(filePathName.text());
        return success;
    }

    void Chain::updateBlockProgress(const Hash &pHash, unsigned int pNodeID, uint32_t pTime)
    {
        mPendingLock.readLock();
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            if((*pending)->block->hash == pHash)
            {
                (*pending)->updateTime = pTime;
                (*pending)->requestingNode = pNodeID;
                break;
            }
        mPendingLock.readUnlock();
    }

    void Chain::markBlocksForNode(HashList &pHashes, unsigned int pNodeID)
    {
        mPendingLock.readLock();
        uint32_t time = getTime();
        for(HashList::iterator hash=pHashes.begin();hash!=pHashes.end();++hash)
            for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
                if((*pending)->block->hash == **hash)
                {
                    (*pending)->requestingNode = pNodeID;
                    (*pending)->requestedTime = time;
                    break;
                }
        mPendingLock.readUnlock();
    }

    void Chain::releaseBlocksForNode(unsigned int pNodeID)
    {
        mPendingLock.readLock();
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
            if(!(*pending)->isFull() && (*pending)->requestingNode == pNodeID)
            {
                (*pending)->requestingNode = 0;
                (*pending)->requestedTime = 0;
            }
        mPendingLock.readUnlock();
    }

    bool Chain::getBlocksNeeded(HashList &pHashes, unsigned int pCount, bool pReduceOnly)
    {
        pHashes.clear();

        mPendingLock.readLock();
        unsigned int offset = 0;
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
        {
            // If "reduce only" don't request blocks unless there is a full pending block after them
            if(pReduceOnly && offset >= mLastFullPendingOffset)
                break;
            ++offset;

            if(!(*pending)->isFull() && (*pending)->requestingNode == 0)
            {
                pHashes.push_back(new Hash((*pending)->block->hash));
                if(pHashes.size() >= pCount)
                    break;
            }
        }
        mPendingLock.readUnlock();

        return pHashes.size() > 0;
    }

    bool Chain::addPendingBlock(Block *pBlock)
    {
        bool success = false;
        bool found = false;
        unsigned int offset = 0;

        mPendingLock.readLock();
        for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
        {
            if((*pending)->block->hash == pBlock->hash)
            {
                found = true;
                if((*pending)->isFull())
                    ArcMist::Log::addFormatted(ArcMist::Log::DEBUG, BITCOIN_CHAIN_LOG_NAME,
                      "Block already received from [%d]: %s", (*pending)->requestingNode, pBlock->hash.hex().text());
                else
                {
                    mPendingSize -= (*pending)->block->size();
                    (*pending)->replace(pBlock);
                    mPendingSize += pBlock->size();
                    mPendingBlocks++;
                    if(offset > mLastFullPendingOffset)
                        mLastFullPendingOffset = offset;
                    success = true;
                }
                break;
            }
            ++offset;
        }
        mPendingLock.readUnlock();

        if(success)
        {
            ArcMist::Log::addFormatted(ArcMist::Log::DEBUG, BITCOIN_CHAIN_LOG_NAME,
              "Added pending block : %s", pBlock->hash.hex().text());
            return true;
        }
        else if(!found)
        {
            // Check if this is the latest block
            mPendingLock.writeLock("Add Block");
            if(pBlock->previousHash == mLastBlockHash && mPending.size() == 0)
            {
                if(!pBlock->hasProofOfWork())
                {
                    mPendingLock.writeUnlock();
                    ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                      "Not enough proof of work : %s", pBlock->hash.hex().text());
                    Hash target;
                    target.setDifficulty(pBlock->targetBits);
                    ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                      "Target                   : %s", target.hex().text());
                    return false;
                }

                // Add to pending list
                mLastFullPendingOffset = mPending.size();
                mPending.push_back(new PendingData(pBlock));
                mLastPendingHash = pBlock->hash;
                mPendingSize += pBlock->size();
                mPendingBlocks++;
                mPendingLock.writeUnlock();
                return true;
            }
            else
            {
                mPendingLock.writeUnlock();
                if(blockInChain(pBlock->hash))
                    ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                      "Received block already in chain : %s", pBlock->hash.hex().text());
                else
                    ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                      "Received unknown block : %s", pBlock->hash.hex().text());
                return false;
            }
        }

        return false;
    }

    bool Chain::processBlock(Block *pBlock)
    {
#ifdef PROFILER_ON
        ArcMist::Profiler outputsProfiler("Chain Process Block");
#endif
        mProcessMutex.lock();

        mBlockProcessStartTime = getTime();

        // Check target bits
        bool useTestMinDifficulty = network() == TESTNET && pBlock->time - mLastBlockTime > 1200;
        updateTargetBits(mNextBlockHeight, pBlock->time, pBlock->targetBits);
        if(pBlock->targetBits != mTargetBits)
        {
            // If on TestNet and 20 minutes since last block
            if(useTestMinDifficulty && pBlock->targetBits == 0x1d00ffff)
            {
                ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                  "Using TestNet special minimum difficulty rule 1d00ffff for block %d", mNextBlockHeight);
            }
            else
            {
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                  "Block target bits don't match chain's current target bits : chain %08x != block %08x",
                  mTargetBits, pBlock->targetBits);
                revertTargetBits(mNextBlockHeight);
                mProcessMutex.unlock();
                return false;
            }
        }

        mBlockStats.push_back(BlockStat(pBlock->version, pBlock->time, pBlock->targetBits));
        mForks.process(mBlockStats, mNextBlockHeight);

        // Process block
        if(!pBlock->process(mOutputs, mNextBlockHeight, mBlockStats, mForks))
        {
            revertTargetBits(mNextBlockHeight);
            mOutputs.revert(mNextBlockHeight);
            mBlockStats.revert(mNextBlockHeight);
            mForks.revert(mBlockStats, mNextBlockHeight);
            mProcessMutex.unlock();
            return false;
        }

        // Add the block to the chain
        bool success = true;
        if(mLastFileID == INVALID_FILE_ID)
        {
            // Create first block file
            mLastFileID = 0;
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Creating first block file %08x", mLastFileID, mLastFileID + 1);
            BlockFile::lock(mLastFileID);
            mLastBlockFile = BlockFile::create(mLastFileID);
            if(mLastBlockFile == NULL) // Failed to create file
                success = false;
        }
        else
        {
            // Check if last block file is full
            BlockFile::lock(mLastFileID);
            if(mLastBlockFile == NULL)
                mLastBlockFile = new BlockFile(mLastFileID);

            if(!mLastBlockFile->isValid())
            {
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                  "Block file %08x is invalid", mLastFileID);

                success = false;
                BlockFile::unlock(mLastFileID);
                delete mLastBlockFile;
            }
            else if(mLastBlockFile->isFull())
            {
                ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                  "Block file %08x is full. Starting new block file %08x", mLastFileID, mLastFileID + 1);

                BlockFile::unlock(mLastFileID);
                delete mLastBlockFile;

                // Create next file
                mLastFileID++;
                BlockFile::lock(mLastFileID);
                mLastBlockFile = BlockFile::create(mLastFileID);
                if(mLastBlockFile == NULL) // Failed to create file
                    success = false;
            }
        }

        if(success)
        {
            success = mLastBlockFile->addBlock(*pBlock);
            BlockFile::unlock(mLastFileID);
        }

        // Commit and save changes to transaction output pool
        if(success && !mOutputs.commit(pBlock->transactions, mNextBlockHeight))
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed to commit transaction outputs to pool");
            revertTargetBits(mNextBlockHeight);
            mOutputs.revert(mNextBlockHeight);
            mBlockStats.revert(mNextBlockHeight);
            mForks.revert(mBlockStats, mNextBlockHeight);
            mProcessMutex.unlock();
            return false;
        }

        if(success)
        {
            uint16_t lookup = pBlock->hash.lookup16();
            mBlockLookup[lookup].lock();
            mBlockLookup[lookup].push_back(new BlockInfo(pBlock->hash, mLastFileID, mNextBlockHeight));
            mBlockLookup[lookup].unlock();

            ++mNextBlockHeight;
            mLastBlockHash = pBlock->hash;
            mLastTargetBits = pBlock->targetBits;

            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Added block to chain at height %d (%d trans) (%d bytes) (%d s) : %s",
              mNextBlockHeight - 1, pBlock->transactionCount, pBlock->size(), getTime() - mBlockProcessStartTime,
              pBlock->hash.hex().text());
        }
        else
        {
            mBlockStats.revert(mNextBlockHeight);
            mForks.revert(mBlockStats, mNextBlockHeight);
            revertTargetBits(mNextBlockHeight);
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed to add block to file %08x : %s", mLastFileID, pBlock->hash.hex().text());
        }

        mProcessMutex.unlock();
        return success;
    }

    void Chain::process()
    {
#ifdef PROFILER_ON
        ArcMist::Profiler outputsProfiler("Chain Process");
#endif
        if(mStop)
            return;

        // Check if first pending header is actually a full block and process it
        mPendingLock.readLock();
        if(mPending.size() == 0)
        {
            // No pending blocks or headers
            mPendingLock.readUnlock();
            if(mLastBlockFile != NULL)
                mLastBlockFile->updateCRC();
            mForks.save();
            return;
        }

        PendingData *nextPending = mPending.front();
        mPendingLock.readUnlock();
        if(!nextPending->isFull()) // Next pending block is not full yet
        {
            if(mLastBlockFile != NULL)
                mLastBlockFile->updateCRC();
            mForks.save();
            return;
        }

        // Check this front block and add it to the chain
        if(processBlock(nextPending->block))
        {
            mPendingLock.writeLock("Process");

            mPendingSize -= nextPending->block->size();
            mPendingBlocks--;

            // Delete block
            delete nextPending;

            // Remove from pending
            mPending.erase(mPending.begin());
            if(mPending.size() == 0)
                mLastPendingHash.clear();
            if(mLastFullPendingOffset > 0)
                --mLastFullPendingOffset;

            mPendingLock.writeUnlock();
        }
        else
        {
            //TODO Add hash to blacklist. So it isn't downloaded again.

            if(mLastBlockFile != NULL)
            {
                delete mLastBlockFile;
                mLastBlockFile = NULL;
            }

            // ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Clearing all pending blocks/headers");

            // // Clear pending blocks since they assumed this block was good
            // mPendingLock.readLock();
            // for(std::list<PendingData *>::iterator pending=mPending.begin();pending!=mPending.end();++pending)
                // delete *pending;
            // mPending.clear();
            // mLastPendingHash.clear();
            // mLastFullPendingOffset = 0;
            // mPendingSize = 0;
            // mPendingBlocks = 0;
            // mPendingLock.readUnlock();

            //TODO Black list block hash

            //TODO Figure out how to recover from this

            // Stop daemon
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Stopping daemon because this is currently unrecoverable");
            Daemon::instance().requestStop();
            mStop = true;
        }
    }

    bool Chain::getBlockHashes(HashList &pHashes, const Hash &pStartingHash, unsigned int pCount)
    {
        BlockFile *blockFile;
        unsigned int fileID;
        HashList fileList;
        bool started = false;

        if(pStartingHash.isEmpty())
        {
            started = true;
            fileID = 0;
        }
        else
            fileID = blockFileID(pStartingHash);

        if(fileID == INVALID_FILE_ID)
            return false;

        pHashes.clear();

        while(pHashes.size() < pCount)
        {
            BlockFile::lock(fileID);
            if(fileID == mLastFileID && mLastBlockFile != NULL)
                blockFile = mLastBlockFile;
            else
                blockFile = new BlockFile(fileID);

            if(!blockFile->readBlockHashes(fileList))
            {
                if(blockFile != mLastBlockFile)
                    delete blockFile;
                BlockFile::unlock(fileID);
                break;
            }

            if(blockFile != mLastBlockFile)
                delete blockFile;
            BlockFile::unlock(fileID);

            for(HashList::iterator i=fileList.begin();i!=fileList.end();)
                if(started || **i == pStartingHash)
                {
                    started = true;
                    pHashes.push_back(*i);
                    i = fileList.erase(i);
                    if(pHashes.size() >= pCount)
                        break;
                }
                else
                    ++i;

            if(pHashes.size() >= pCount)
                break;
            if(++fileID > mLastFileID)
                break;
        }

        return pHashes.size() > 0;
    }

    void Chain::getReverseBlockHashes(HashList &pHashes, unsigned int pCount)
    {
        BlockFile *blockFile;
        Hash hash;

        pHashes.clear();

        // Don't start with latest block. Go back to previous file
        if(mLastFileID == 0)
            return;

        for(unsigned int fileID=mLastFileID-1;;fileID--)
        {
            BlockFile::lock(fileID);
            if(fileID == mLastFileID && mLastBlockFile != NULL)
                blockFile = mLastBlockFile;
            else
                blockFile = new BlockFile(fileID);

            hash = blockFile->lastHash();
            if(!hash.isEmpty())
                pHashes.push_back(new Hash(hash));

            if(blockFile != mLastBlockFile)
                delete blockFile;
            BlockFile::unlock(fileID);

            if(pHashes.size() >= pCount || fileID == 0)
                break;
        }
    }

    bool Chain::getBlockHeaders(BlockList &pBlockHeaders, const Hash &pStartingHash, const Hash &pStoppingHash, unsigned int pCount)
    {
        BlockFile *blockFile;
        Hash hash = pStartingHash;
        unsigned int fileID = blockFileID(hash);

        pBlockHeaders.clear();

        if(fileID == INVALID_FILE_ID)
            return false; // hash not found

        while(pBlockHeaders.size() < pCount)
        {
            BlockFile::lock(fileID);
            if(fileID == mLastFileID && mLastBlockFile != NULL)
                blockFile = mLastBlockFile;
            else
                blockFile = new BlockFile(fileID);

            if(!blockFile->isValid() || !blockFile->readBlockHeaders(pBlockHeaders, hash, pStoppingHash, pCount))
            {
                if(blockFile != mLastBlockFile)
                    delete blockFile;
                BlockFile::unlock(fileID);
                break;
            }

            if(blockFile != mLastBlockFile)
                delete blockFile;
            BlockFile::unlock(fileID);

            if(pBlockHeaders.size() > 0 && pBlockHeaders.back()->hash == pStoppingHash)
                break;

            hash.clear();
            if(++fileID > mLastFileID)
                break;
        }

        return pBlockHeaders.size() > 0;
    }

    bool Chain::getBlockHash(unsigned int pHeight, Hash &pHash)
    {
        unsigned int fileID = pHeight / 100;
        unsigned int offset = pHeight - (fileID * 100);

        if(fileID > mLastFileID)
            return false;

        BlockFile *blockFile;

        BlockFile::lock(fileID);
        if(fileID == mLastFileID && mLastBlockFile != NULL)
            blockFile = mLastBlockFile;
        else
            blockFile = new BlockFile(fileID);

        bool success = blockFile->isValid() && blockFile->readHash(offset, pHash);

        if(blockFile != mLastBlockFile)
            delete blockFile;
        BlockFile::unlock(fileID);
        return success;
    }

    bool Chain::getBlock(unsigned int pHeight, Block &pBlock)
    {
        unsigned int fileID = pHeight / 100;
        unsigned int offset = pHeight - (fileID * 100);

        if(fileID > mLastFileID)
            return false;

        BlockFile *blockFile;

        BlockFile::lock(fileID);
        if(fileID == mLastFileID && mLastBlockFile != NULL)
            blockFile = mLastBlockFile;
        else
            blockFile = new BlockFile(fileID);

        bool success = blockFile->isValid() && blockFile->readBlock(offset, pBlock, true);

        if(blockFile != mLastBlockFile)
            delete blockFile;
        BlockFile::unlock(fileID);
        return success;
    }

    bool Chain::getBlock(const Hash &pHash, Block &pBlock)
    {
        unsigned int fileID = blockFileID(pHash);
        if(fileID == INVALID_FILE_ID)
            return false; // hash not found
        BlockFile *blockFile;

        BlockFile::lock(fileID);
        if(fileID == mLastFileID && mLastBlockFile != NULL)
            blockFile = mLastBlockFile;
        else
            blockFile = new BlockFile(fileID);

        bool success = blockFile->isValid() && blockFile->readBlock(pHash, pBlock, true);

        if(blockFile != mLastBlockFile)
            delete blockFile;
        BlockFile::unlock(fileID);
        return success;
    }

    bool Chain::getHeader(const Hash &pHash, Block &pBlockHeader)
    {
        unsigned int fileID = blockFileID(pHash);
        if(fileID == INVALID_FILE_ID)
            return false; // hash not found
        BlockFile *blockFile;

        BlockFile::lock(fileID);
        if(fileID == mLastFileID && mLastBlockFile != NULL)
            blockFile = mLastBlockFile;
        else
            blockFile = new BlockFile(fileID);

        bool success = blockFile->isValid() && blockFile->readBlock(pHash, pBlockHeader, false);

        if(blockFile != mLastBlockFile)
            delete blockFile;
        BlockFile::unlock(fileID);
        return success;
    }

    bool Chain::updateOutputs()
    {
        int height = mOutputs.blockHeight();
        if(height == blockHeight())
            return true;

        height++;

        Hash startHash;
        if(!getBlockHash(height, startHash))
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed to get next block to update unspent transaction outputs");
            return false;
        }

        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
          "Updating unspent transaction outputs from block height %d to %d", height, blockHeight());

        //TODO This soft fork data matches the block chain height and not the unspent height.
        //  It needs to be "backed up" to the unspent height.

        ArcMist::String filePathName;
        unsigned int fileID = blockFileID(startHash);
        HashList hashes;
        BlockFile *blockFile = NULL;
        Block block;
        unsigned int blockOffset;
        Forks emptyForks;
        uint32_t lastPurgeTime = getTime();

        while(!mStop)
        {
            filePathName = BlockFile::fileName(fileID);
            if(ArcMist::fileExists(filePathName))
            {
                BlockFile::lock(fileID);
                blockFile = new BlockFile(fileID);
                if(!blockFile->isValid())
                {
                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                      "Block file %08x is invalid", fileID);
                    delete blockFile;
                    BlockFile::unlock(fileID);
                    return false;
                }

                if(!blockFile->readBlockHashes(hashes))
                {
                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                      "Failed to read hashes from block file %08x", fileID);
                    delete blockFile;
                    BlockFile::unlock(fileID);
                    return false;
                }
                BlockFile::unlock(fileID);

                blockOffset = 0;
                for(HashList::iterator hash=hashes.begin();hash!=hashes.end();++hash)
                {
                    if(startHash.isEmpty() || **hash == startHash)
                    {
                        startHash.clear();
                        BlockFile::lock(fileID);
                        if(blockFile->readBlock(blockOffset, block, true))
                        {
                            mBlockProcessStartTime = getTime();

                            BlockFile::unlock(fileID);
                            if(block.updateOutputs(mOutputs, height))
                            {
                                ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                                  "Processed block %d (%d trans) (%d bytes) (%d s) : %s", height, block.transactionCount,
                                  block.size(), getTime() - mBlockProcessStartTime, block.hash.hex().text());
                                mOutputs.commit(block.transactions, height++);
                                if(getTime() - lastPurgeTime > 300)
                                {
                                    mOutputs.purge();
                                    lastPurgeTime = getTime();
                                }
                            }
                            else
                            {
                                mOutputs.revert(height);
                                mOutputs.save();
                                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                                  "Failed to process block at height %d. At offset %d in block file %08x : %s",
                                  height, blockOffset, fileID, (*hash)->hex().text());
                                delete blockFile;
                                return false;
                            }
                        }
                        else
                        {
                            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                              "Failed to read block %d from block file %08x : %s", blockOffset, fileID, (*hash)->hex().text());
                            delete blockFile;
                            BlockFile::unlock(fileID);
                            mOutputs.save();
                            return false;
                        }
                    }

                    ++blockOffset;

                    if(mStop)
                        break;
                }

                delete blockFile;
            }
            else
                break;

            fileID++;
        }

        mOutputs.save();
        return mOutputs.blockHeight() == blockHeight();
    }

    bool Chain::save()
    {
        if(mLastBlockFile != NULL)
            delete mLastBlockFile;
        mLastBlockFile = NULL;
        bool success = true;
        if(!mBlockStats.save())
            success = false;
        if(!mForks.save())
            success = false;
        if(!savePending())
            success = false;
        if(!mOutputs.save())
            success = false;
        return success;
    }

    // Load block info from files
    bool Chain::load(bool pList)
    {
        ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Indexing block hashes");

        BlockFile *blockFile = NULL;
        uint16_t lookup;
        ArcMist::String filePathName;
        HashList hashes;
        Hash *lastBlock = NULL;
        bool success = true;
        Hash emptyHash;
        unsigned int fileID;

        mProcessMutex.lock();

        mLastFileID = INVALID_FILE_ID;
        mNextBlockHeight = 0;
        mLastBlockHash.setSize(32);
        mLastBlockHash.zeroize();

        for(fileID=0;;fileID++)
        {
            BlockFile::lock(fileID);
            filePathName = BlockFile::fileName(fileID);
            if(ArcMist::fileExists(filePathName))
            {
                blockFile = new BlockFile(fileID, false);
                if(!blockFile->isValid())
                {
                    delete blockFile;
                    BlockFile::unlock(fileID);
                    success = false;
                    break;
                }

                if(!blockFile->readBlockHashes(hashes))
                {
                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                      "Failed to read hashes from block file %08x", fileID);
                    delete blockFile;
                    BlockFile::unlock(fileID);
                    success = false;
                    break;
                }
                delete blockFile;
                BlockFile::unlock(fileID);

                if(pList)
                    ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                      "Block file %08x", fileID);

                mLastFileID = fileID;
                for(HashList::iterator hash=hashes.begin();hash!=hashes.end();++hash)
                {
                    if(pList)
                        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                          "Block %d : %s", mNextBlockHeight, (*hash)->hex().text());
                    lookup = (*hash)->lookup16();
                    mBlockLookup[lookup].lock();
                    mBlockLookup[lookup].push_back(new BlockInfo(**hash, fileID, mNextBlockHeight));
                    mBlockLookup[lookup].unlock();
                    mNextBlockHeight++;
                    lastBlock = *hash;
                }
            }
            else
            {
                BlockFile::unlock(fileID);
                break;
            }
        }

        if(success)
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Indexed %d block hashes", mNextBlockHeight);

        if(success && !loadTargetBits())
            success = false;

        if(success && !mBlockStats.load())
            success = false;

        if(success)
        {
            if(mBlockStats.height() > mNextBlockHeight)
                mBlockStats.resize(mNextBlockHeight);

            if(mBlockStats.height() < mNextBlockHeight - 1)
            {
                ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                  "Refreshing block statistics (height %d)", mBlockStats.height());

                mBlockStats.clear();
                mBlockStats.reserve(mNextBlockHeight);
                uint32_t lastReport = getTime();
                for(fileID=0;fileID<=mLastFileID;fileID++)
                {
                    if(getTime() - lastReport > 10)
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                          "Block statistics load is %2d%% Complete", (int)(((float)fileID / (float)mLastFileID) * 100.0f));
                        lastReport = getTime();
                    }

                    BlockFile::lock(fileID);
                    blockFile = new BlockFile(fileID, false);
                    if(!blockFile->isValid())
                    {
                        delete blockFile;
                        BlockFile::unlock(fileID);
                        success = false;
                        break;
                    }

                    if(!blockFile->readStats(mBlockStats))
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Failed to read hashes from block file %08x", fileID);
                        delete blockFile;
                        BlockFile::unlock(fileID);
                        success = false;
                        break;
                    }
                    delete blockFile;
                    BlockFile::unlock(fileID);
                }

                if(success)
                    mBlockStats.save();
            }
        }

        success = success && mForks.load();

        if(success && mForks.height() != mNextBlockHeight - 1)
        {
            ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
              "Refreshing soft forks (height %d)", mForks.height());

            mForks.reset();
            uint32_t lastReport = getTime();
            for(int i=mForks.height()+1;i<mNextBlockHeight;++i)
            {
                if(getTime() - lastReport > 10)
                {
                    ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                      "Soft forks load is %2d%% Complete", (int)(((float)i / (float)mNextBlockHeight) * 100.0f));
                    lastReport = getTime();
                }

                mForks.process(mBlockStats, i);
            }

            if(mStop)
                success = false;

            if(success)
                mForks.save();
        }

        mProcessMutex.unlock();

        // Load transaction outputs
        success = success && mOutputs.load();

        // Update transaction outputs if they aren't up to current chain block height
        success = success && updateOutputs();

        if(success)
        {
            if(mNextBlockHeight == 0)
            {
                // Add genesis block
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Creating genesis block");
                Block *genesis = Block::genesis();
                bool success = processBlock(genesis);
                delete genesis;
                if(!success)
                    return false;
            }

            if(lastBlock != NULL)
                mLastBlockHash = *lastBlock;
        }

        return success && loadPending();
    }

    bool Chain::validate(bool pRebuild)
    {
        BlockFile *blockFile;
        Hash previousHash(32), merkleHash;
        Block block;
        unsigned int i, height = 0;
        bool useTestMinDifficulty;
        ArcMist::String filePathName;

        for(unsigned int fileID=0;!mStop;fileID++)
        {
            filePathName = BlockFile::fileName(fileID);
            if(!ArcMist::fileExists(filePathName))
                break;

            BlockFile::lock(fileID);
            blockFile = new BlockFile(fileID);

            if(!blockFile->isValid())
            {
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                  "Block file %08x isn't valid", fileID);
                break;
            }

            for(i=0;i<BlockFile::MAX_BLOCKS;i++)
            {
                if(blockFile->readBlock(i, block, true))
                {
                    if(block.previousHash != previousHash)
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Block %010d previous hash doesn't match", height);
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Included Previous Hash : %s", block.previousHash.hex().text());
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Previous Block's Hash  : %s", previousHash.hex().text());
                        return false;
                    }

                    block.calculateMerkleHash(merkleHash);
                    if(block.merkleHash != merkleHash)
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Block %010d has invalid merkle hash", height);
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Included Merkle Hash : %s", block.merkleHash.hex().text());
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Correct Merkle Hash  : %s", merkleHash.hex().text());
                        return false;
                    }

                    useTestMinDifficulty = network() == TESTNET && block.time - mLastBlockTime > 1200;
                    updateTargetBits(height, block.time, block.targetBits);
                    mBlockStats.push_back(BlockStat(block.version, block.time, block.targetBits));
                    mForks.process(mBlockStats, height);
                    if(mTargetBits != block.targetBits)
                    {
                        // If on TestNet and 20 minutes since last block
                        if(useTestMinDifficulty && block.targetBits == 0x1d00ffff)
                        {
                            ArcMist::Log::addFormatted(ArcMist::Log::VERBOSE, BITCOIN_CHAIN_LOG_NAME,
                              "Using TestNet special minimum difficulty rule 1d00ffff for block %d", height);
                        }
                        else
                        {
                            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                              "Block %010d target bits don't match chain's current target bits : chain %08x != block %08x",
                              height, mTargetBits, block.targetBits);
                            return false;
                        }
                    }

                    if(!block.process(mOutputs, height, mBlockStats, mForks))
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Block %010d failed to process", height);
                        return false;
                    }

                    if(!mOutputs.commit(block.transactions, height))
                    {
                        ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                          "Block %010d unspent transaction outputs commit failed", height);
                        return false;
                    }

                    ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                      "Block %010d is valid : %6d trans, %d bytes", height, block.transactions.size(), block.size());
                    //block.print();

                    previousHash = block.hash;
                    height++;
                }
                else // End of chain
                    break;
            }

            delete blockFile;
            BlockFile::unlock(fileID);
        }

        if(pRebuild)
        {
            mOutputs.save();
            if(!mForks.save())
                return false;
            if(!saveTargetBits())
                return false;
        }

        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
          "Unspent transactions/outputs : %d/%d", mOutputs.transactionCount(), mOutputs.outputCount());
        ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Validated block height of %d", height);
        return true;
    }

    bool Chain::test()
    {
        ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "------------- Starting Block Chain Tests -------------");

        bool success = true;
        ArcMist::Buffer checkData;
        Hash checkHash(32);
        Block *genesis = Block::genesis();

        //genesis->print(ArcMist::Log::INFO);

        //ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Current coin base amount : %f",
        // (double)bitcoins(Block::coinBaseAmount(485000)));

        /***********************************************************************************************
         * Genesis block merkle hash
         ***********************************************************************************************/
        checkData.clear();
        checkData.writeHex("3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a");
        checkHash.read(&checkData);

        if(genesis->merkleHash == checkHash)
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed genesis block merkle hash");
        else
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed genesis block merkle hash");
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block merkle hash   : %s", genesis->merkleHash.hex().text());
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct merkle hash : %s", checkHash.hex().text());
            success = false;
        }

        /***********************************************************************************************
         * Genesis block hash
         ***********************************************************************************************/
        //Big Endian checkData.writeHex("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
        if(network() == TESTNET)
            checkData.writeHex("43497fd7f826957108f4a30fd9cec3aeba79972084e90ead01ea330900000000");
        else
            checkData.writeHex("6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
        checkHash.read(&checkData);

        if(genesis->hash == checkHash)
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed genesis block hash");
        else
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed genesis block hash");
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block hash   : %s", genesis->hash.hex().text());
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct hash : %s", checkHash.hex().text());
            success = false;
        }

        /***********************************************************************************************
         * Genesis block read hash
         ***********************************************************************************************/
        //Big Endian checkData.writeHex("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
        checkData.clear();
        if(network() == TESTNET)
            checkData.writeHex("43497fd7f826957108f4a30fd9cec3aeba79972084e90ead01ea330900000000");
        else
            checkData.writeHex("6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
        checkHash.read(&checkData);
        Block readGenesisBlock;
        ArcMist::Buffer blockBuffer;
        genesis->write(&blockBuffer, true, true);
        readGenesisBlock.read(&blockBuffer, true, true, true);

        if(readGenesisBlock.hash == checkHash)
            ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed genesis block read hash");
        else
        {
            ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed genesis block read hash");
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block hash   : %s", readGenesisBlock.hash.hex().text());
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct hash : %s", checkHash.hex().text());
            success = false;
        }

        /***********************************************************************************************
         * Genesis block raw
         ***********************************************************************************************/
        ArcMist::Buffer data;
        genesis->write(&data, true, true);

        checkData.clear();
        if(network() == TESTNET)
        {
            checkData.writeHex("01000000000000000000000000000000"); //   ................
            checkData.writeHex("00000000000000000000000000000000"); //   ................
            checkData.writeHex("000000003BA3EDFD7A7B12B27AC72C3E"); //   ....;£íýz{.²zÇ,>
            checkData.writeHex("67768F617FC81BC3888A51323A9FB8AA"); //   gv.a.È.ÃˆŠQ2:Ÿ¸ª
            checkData.writeHex("4b1e5e4adae5494dffff001d1aa4ae18"); //   <CHANGED>
            checkData.writeHex("01010000000100000000000000000000"); //   ................
            checkData.writeHex("00000000000000000000000000000000"); //   ................
            checkData.writeHex("000000000000FFFFFFFF4D04FFFF001D"); //   ......ÿÿÿÿM.ÿÿ..
            checkData.writeHex("0104455468652054696D65732030332F"); //   ..EThe Times 03/
            checkData.writeHex("4A616E2F32303039204368616E63656C"); //   Jan/2009 Chancel
            checkData.writeHex("6C6F72206F6E206272696E6B206F6620"); //   lor on brink of
            checkData.writeHex("7365636F6E64206261696C6F75742066"); //   second bailout f
            checkData.writeHex("6F722062616E6B73FFFFFFFF0100F205"); //   or banksÿÿÿÿ..ò.
            checkData.writeHex("2A01000000434104678AFDB0FE554827"); //   *....CA.gŠý°þUH'
            checkData.writeHex("1967F1A67130B7105CD6A828E03909A6"); //   .gñ¦q0·.\Ö¨(à9.¦
            checkData.writeHex("7962E0EA1F61DEB649F6BC3F4CEF38C4"); //   ybàê.aÞ¶Iö¼?Lï8Ä
            checkData.writeHex("F35504E51EC112DE5C384DF7BA0B8D57"); //   óU.å.Á.Þ\8M÷º..W
            checkData.writeHex("8A4C702B6BF11D5FAC00000000");       //   ŠLp+kñ._¬....
        }
        else
        {
            checkData.writeHex("01000000000000000000000000000000"); //   ................
            checkData.writeHex("00000000000000000000000000000000"); //   ................
            checkData.writeHex("000000003BA3EDFD7A7B12B27AC72C3E"); //   ....;£íýz{.²zÇ,>
            checkData.writeHex("67768F617FC81BC3888A51323A9FB8AA"); //   gv.a.È.ÃˆŠQ2:Ÿ¸ª
            checkData.writeHex("4B1E5E4A29AB5F49FFFF001D1DAC2B7C"); //   K.^J)«_Iÿÿ...¬+|
            checkData.writeHex("01010000000100000000000000000000"); //   ................
            checkData.writeHex("00000000000000000000000000000000"); //   ................
            checkData.writeHex("000000000000FFFFFFFF4D04FFFF001D"); //   ......ÿÿÿÿM.ÿÿ..
            checkData.writeHex("0104455468652054696D65732030332F"); //   ..EThe Times 03/
            checkData.writeHex("4A616E2F32303039204368616E63656C"); //   Jan/2009 Chancel
            checkData.writeHex("6C6F72206F6E206272696E6B206F6620"); //   lor on brink of
            checkData.writeHex("7365636F6E64206261696C6F75742066"); //   second bailout f
            checkData.writeHex("6F722062616E6B73FFFFFFFF0100F205"); //   or banksÿÿÿÿ..ò.
            checkData.writeHex("2A01000000434104678AFDB0FE554827"); //   *....CA.gŠý°þUH'
            checkData.writeHex("1967F1A67130B7105CD6A828E03909A6"); //   .gñ¦q0·.\Ö¨(à9.¦
            checkData.writeHex("7962E0EA1F61DEB649F6BC3F4CEF38C4"); //   ybàê.aÞ¶Iö¼?Lï8Ä
            checkData.writeHex("F35504E51EC112DE5C384DF7BA0B8D57"); //   óU.å.Á.Þ\8M÷º..W
            checkData.writeHex("8A4C702B6BF11D5FAC00000000");       //   ŠLp+kñ._¬....
        }

        if(checkData.length() != data.length())
        {
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              "Failed genesis block raw data size : actual %d != correct %d", data.length(), checkData.length());
            success = false;
        }
        else
        {
            // Check in 16 byte sections
            uint8_t actualRaw[16], checkRaw[16];
            ArcMist::String actualHex, checkHex;
            bool matches = true;
            for(unsigned int lineNo=1;checkData.remaining() > 0;lineNo++)
            {
                data.read(actualRaw, 16);
                checkData.read(checkRaw, 16);

                if(std::memcmp(actualRaw, checkRaw, 16) != 0)
                {
                    matches = false;
                    actualHex.writeHex(actualRaw, 16);
                    checkHex.writeHex(checkRaw, 16);

                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed genesis block raw data line %d", lineNo);
                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Actual  : %s", actualHex.text());
                    ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct : %s", checkHex.text());
                    success = false;
                }
            }

            if(matches)
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed genesis block raw data");
        }

        /***********************************************************************************************
         * Block read
         ***********************************************************************************************/
        Block readBlock;
        ArcMist::FileInputStream readFile("tests/06128e87be8b1b4dea47a7247d5528d2702c96826c7a648497e773b800000000.pending_block");
        Info::instance().setPath("../bcc_test");
        TransactionOutputPool outputs;
        BlockStats blockStats;
        Forks softForks;

        outputs.load();

        if(!readBlock.read(&readFile, true, true, true))
        {
            ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed to read block");
            success = false;
        }
        else
        {
            //readBlock.print(ArcMist::Log::INFO);

            /***********************************************************************************************
             * Block read hash
             ***********************************************************************************************/
            checkData.clear();
            checkData.writeHex("06128e87be8b1b4dea47a7247d5528d2702c96826c7a648497e773b800000000");
            checkHash.read(&checkData);

            if(readBlock.hash == checkHash)
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed read block hash");
            else
            {
                ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed read block hash");
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block hash   : %s", readBlock.hash.hex().text());
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct hash : %s", checkHash.hex().text());
                success = false;
            }

            /***********************************************************************************************
             * Block read previous hash
             ***********************************************************************************************/
            checkData.clear();
            checkData.writeHex("43497fd7f826957108f4a30fd9cec3aeba79972084e90ead01ea330900000000");
            checkHash.read(&checkData);

            if(readBlock.previousHash == checkHash)
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed read block previous hash");
            else
            {
                ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed read block previous hash");
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block previous hash   : %s", readBlock.previousHash.hex().text());
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Correct previous hash : %s", checkHash.hex().text());
                success = false;
            }

            /***********************************************************************************************
             * Block read merkle hash
             ***********************************************************************************************/
            readBlock.calculateMerkleHash(checkHash);

            if(readBlock.merkleHash == checkHash)
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed read block merkle hash");
            else
            {
                ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed read block merkle hash");
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Block merkle hash      : %s", readBlock.merkleHash.hex().text());
                ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Calculated merkle hash : %s", checkHash.hex().text());
                success = false;
            }

            /***********************************************************************************************
             * Block read process
             ***********************************************************************************************/
            if(readBlock.process(outputs, 0, blockStats, softForks))
                ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed read block process");
            else
            {
                ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed read block process");
                success = false;
            }
        }

        delete genesis;
        return success;
    }

    void Chain::tempTest()
    {
        // TransactionOutputPool outputs;
        // // BlockStats blockStats;
        // // Forks softForks;

        // // ArcMist::Log::setOutputFile("convert.log");
        // Info::instance().setPath("/var/bitcoin/mainnet");

        // // blockStats.load();
        // // softForks.load();
        // outputs.load(false);
        // // // outputs.revert(388700, true);
        // // // outputs.save();

        // outputs.convert();


        // ArcMist::FileInputStream file("/var/bitcoin/mainnet/pending");
        // Block block;

        // // BlockFile::readBlock(386340, block);

        // if(!block.read(&file, true, true, true))
        // {
            // ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed to read pending block");
            // return;
        // }

        // if(block.process(outputs, outputs.blockHeight() + 1, blockStats, softForks))
            // ArcMist::Log::add(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME, "Passed pending block");
        // else
            // ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME, "Failed pending block");



        // Hash hash("ffffc1856901838e7128469d3f3d53bfc0d3cc83f647af242da7a37d7dda158d");
        // unsigned int index = 0;

        // // // Load transactions from block
        // // outputs.add(block.transactions, outputs.blockHeight() + 1, block.hash);

        // // // Check for matching transaction in block
        // // for(std::vector<Transaction *>::iterator tran=block.transactions.begin();tran!=block.transactions.end();++tran)
            // // if((*tran)->hash == hash)
                // // ArcMist::Log::addFormatted(ArcMist::Log::INFO, BITCOIN_CHAIN_LOG_NAME,
                  // // "Added transaction : %s", hash.hex().text());

        // TransactionReference *reference = outputs.findUnspent(hash, index);
        // Output output;
        // if(reference != NULL)
        // {
            // reference->print();

            // // if((int)reference->blockHeight == outputs.blockHeight())
            // // {
                // // for(std::vector<Transaction *>::iterator tran=block.transactions.begin();tran!=block.transactions.end();++tran)
                    // // if((*tran)->hash == reference->id)
                    // // {
                        // // unsigned int outputIndex = 0;
                        // // for(std::vector<Output *>::iterator item=(*tran)->outputs.begin();item!=(*tran)->outputs.end();++item)
                        // // {
                            // // if(outputIndex == index)
                            // // {
                                // // output = **item;
                                // // output.print();
                                // // break;
                            // // }
                            // // ++outputIndex;
                        // // }
                    // // }
            // // }
            // // else
            // // {
                // if(BlockFile::readOutput(reference, index, output))
                    // output.print();
                // else
                    // ArcMist::Log::add(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
                      // "Failed to read output for transaction");
            // // }
        // }
        // else
            // ArcMist::Log::addFormatted(ArcMist::Log::ERROR, BITCOIN_CHAIN_LOG_NAME,
              // "Failed to find transaction : %s", hash.hex().text());


        // block.print(ArcMist::Log::INFO, false);






        // Info::instance().setPath("/var/bitcoin/mainnet");
        // Chain chain;

        // chain.load(false);

        // for(int i=0;i<5;++i)
            // chain.process();

        // chain.save();



#ifdef PROFILER_ON
        ArcMist::String profilerTime;
        profilerTime.writeFormattedTime(getTime(), "%Y%m%d.%H%M");
        ArcMist::String profilerFileName = "profiler.";
        profilerFileName += profilerTime;
        profilerFileName += ".txt";
        ArcMist::FileOutputStream profilerFile(profilerFileName, true);
        ArcMist::ProfilerManager::write(&profilerFile);
#endif
    }
}
