/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "block.hpp"

#ifdef PROFILER_ON
#include "profiler.hpp"
#include "profiler_setup.hpp"
#endif

#include "log.hpp"
#include "endian.hpp"
#include "thread.hpp"
#include "digest.hpp"
#include "interpreter.hpp"
#include "info.hpp"
#include "header.hpp"
#include "chain.hpp"

#define BITCOIN_BLOCK_LOG_NAME "Block"


namespace BitCoin
{
    uint64_t Block::actualCoinbaseAmount() const
    {
        if(transactions.size() == 0)
            return 0UL;

        uint64_t result = 0UL;
        TransactionReference coinbase = transactions.front();
        for(std::vector<Output>::const_iterator output = coinbase->outputs.begin();
          output != coinbase->outputs.end(); ++output)
            result += output->amount;

        return result;
    }

    void Block::write(NextCash::OutputStream *pStream)
    {
        NextCash::stream_size startOffset = pStream->writeOffset();
        mSize = 0;

        header.write(pStream, false);

        writeCompactInteger(pStream, transactions.size());

        // Transactions
        for(TransactionList::iterator trans = transactions.begin();
          trans != transactions.end(); ++trans)
            (*trans)->write(pStream);

        mSize = pStream->writeOffset() - startOffset;
    }

    bool Block::read(NextCash::InputStream *pStream)
    {
#ifdef PROFILER_ON
        NextCash::Profiler &profiler = NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_READ_ID, PROFILER_BLOCK_READ_NAME);
        NextCash::ProfilerReference profilerRef(profiler, true);
#endif

        NextCash::stream_size startOffset = pStream->readOffset();
        mSize = 0;

        if(!header.read(pStream, true))
            return false;

        transactions.clear();
        if(header.transactionCount > MAX_BLOCK_TRANSACTIONS)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Block read failed. Too many transactions : %d", header.transactionCount);
              return false;
        }

        // Transactions
        Transaction *transaction;
        transactions.reserve(header.transactionCount);
        for(unsigned int i = 0; i < header.transactionCount; ++i)
        {
            transaction = new Transaction();
            if(transaction->read(pStream))
                transactions.emplace_back(transaction);
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
                  "Block read failed : transaction %d read failed", i + 1);
                delete transaction;
                return false;
            }
        }

        mSize = pStream->readOffset() - startOffset;
#ifdef PROFILER_ON
        profiler.addHits(size() - 1); // One hit (byte) will be added by reference.
#endif
        return true;
    }

    void Block::clear()
    {
        header.clear();
        transactions.clear();
        mFees = 0;
        mSize = 0;
    }

    void Block::print(Forks &pForks, bool pIncludeTransactions, NextCash::Log::Level pLevel)
    {
        header.print(pLevel);

        NextCash::Log::addFormatted(pLevel, BITCOIN_BLOCK_LOG_NAME, "Total Fees    : %f",
          bitcoins(mFees));
        NextCash::Log::addFormatted(pLevel, BITCOIN_BLOCK_LOG_NAME, "Size (KB)     : %d",
          mSize / 1000);

        if(!pIncludeTransactions)
            return;

        unsigned int index = 0;
        for(TransactionList::iterator transaction = transactions.begin();
          transaction != transactions.end(); ++transaction)
        {
            if(index == 0)
                NextCash::Log::addFormatted(pLevel, BITCOIN_BLOCK_LOG_NAME, "Coinbase Transaction",
                  index++);
            else
                NextCash::Log::addFormatted(pLevel, BITCOIN_BLOCK_LOG_NAME, "Transaction %d",
                  index++);
            (*transaction)->print(pForks, pLevel);
        }
    }

    void concatHash(const NextCash::Hash &pLeft, const NextCash::Hash &pRight,
      NextCash::Hash &pResult)
    {
        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        digest.setOutputEndian(NextCash::Endian::LITTLE);
        pLeft.write(&digest);
        pRight.write(&digest);
        pResult.setSize(BLOCK_HASH_SIZE);
        digest.getResult(&pResult);
    }

    void calculateMerkleHashLevel(std::vector<NextCash::Hash> &pHashes, NextCash::Hash &pResult)
    {
        std::vector<NextCash::Hash>::iterator next = pHashes.begin();
        ++next;
        if(next == pHashes.end())
        {
            // Only one entry. Hash it with itself and return
            concatHash(*pHashes.begin(), *pHashes.begin(), pResult);
            return;
        }

        std::vector<NextCash::Hash>::iterator nextNext = next;
        ++nextNext;
        if(nextNext == pHashes.end())
        {
            // Two entries. Hash them together and return
            concatHash(*pHashes.begin(), *next, pResult);
            return;
        }

        // More than two entries. Move up the tree a level.
        std::vector<NextCash::Hash> nextLevel;
        NextCash::Hash one, two, newHash;
        std::vector<NextCash::Hash>::iterator hash = pHashes.begin();

        while(hash != pHashes.end())
        {
            // Get one
            one = *hash++;

            // Get two (first one again if no second)
            if(hash == pHashes.end())
                two = one;
            else
                two = *hash++;

            // Hash these and add to the next level
            concatHash(one, two, newHash);
            nextLevel.push_back(newHash);
        }

        // Clear current level
        pHashes.clear();

        // Calculate the next level
        calculateMerkleHashLevel(nextLevel, pResult);
    }

    void Block::calculateMerkleHash(NextCash::Hash &pMerkleHash)
    {
        pMerkleHash.setSize(BLOCK_HASH_SIZE);
        if(transactions.size() == 0)
            pMerkleHash.zeroize();
        else if(transactions.size() == 1)
            pMerkleHash = transactions.front()->hash();
        else
        {
            // Collect transaction hashes
            std::vector<NextCash::Hash> hashes;
            for(TransactionList::iterator trans = transactions.begin();
              trans != transactions.end(); ++trans)
                hashes.push_back((*trans)->hash());

            // Calculate the next level
            calculateMerkleHashLevel(hashes, pMerkleHash);
        }
    }

    bool MerkleNode::calculateHash()
    {
        if(left == NULL)
        {
            hash.setSize(BLOCK_HASH_SIZE);
            hash.zeroize();
            return true;
        }

        if(left->hash.isEmpty() || right->hash.isEmpty())
            return false;

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        digest.setOutputEndian(NextCash::Endian::LITTLE);
        left->hash.write(&digest);
        right->hash.write(&digest);
        hash.setSize(BLOCK_HASH_SIZE);
        digest.getResult(&hash);
        return true;
    }

    MerkleNode *buildMerkleTreeLevel(std::vector<MerkleNode *> pNodes)
    {
        std::vector<MerkleNode *>::iterator node = pNodes.begin(), left, right;
        std::vector<MerkleNode *>::iterator next = pNodes.begin();
        MerkleNode *newNode;

        ++next;
        if(next == pNodes.end())
        {
            // Only one entry. It is the root.
            return *pNodes.begin();
        }

        ++next;
        if(next == pNodes.end())
        {
            // Only two entries. Combine the hash and return it.
            left = pNodes.begin();
            right = pNodes.begin();
            ++right;
            newNode = new MerkleNode(*left, *right, (*left)->matches || (*right)->matches);
            return newNode;
        }

        // Move up the tree a level.
        std::vector<MerkleNode *> nextLevel;

        while(node != pNodes.end())
        {
            // Get left
            left = node++;

            // Get right, if none remaining use same again
            if(node == pNodes.end())
                right = left;
            else
                right = node++;

            // Hash these and add to the next level
            newNode = new MerkleNode(*left, *right, (*left)->matches || (*right)->matches);
            nextLevel.push_back(newNode);
        }

        // Clear current level
        pNodes.clear();

        // Build the next level
        return buildMerkleTreeLevel(nextLevel);
    }

    MerkleNode *buildMerkleTree(TransactionList &pBlockTransactions, BloomFilter &pFilter)
    {
        if(pBlockTransactions.size() == 0)
            return new MerkleNode(NULL, NULL, false);
        else if(pBlockTransactions.size() == 1)
            return new MerkleNode(pBlockTransactions.front(),
              pFilter.contains(pBlockTransactions.front()));

        // Build leaf nodes
        std::vector<MerkleNode *> nodes;
        for(TransactionList::iterator trans = pBlockTransactions.begin();
          trans != pBlockTransactions.end(); ++trans)
            nodes.push_back(new MerkleNode(*trans, pFilter.contains(*trans)));

        // Calculate the next level
        return buildMerkleTreeLevel(nodes);
    }

    MerkleNode *buildEmptyMerkleTree(unsigned int pNodeCount)
    {
        // Build leaf nodes
        std::vector<MerkleNode *> nodes;
        for(unsigned int i=0;i<pNodeCount;++i)
            nodes.push_back(new MerkleNode());

        return buildMerkleTreeLevel(nodes);
    }

    // bool MerkleNode::calculateHash()
    // {
        // if(!hash.isEmpty())
            // return true;

        // if(left == NULL || right == NULL)
            // return false;
        // if(!left->calculateHash() || !right->calculateHash())
            // return false;
        // calculateHashFromChildren();
    // }

    void MerkleNode::print(unsigned int pDepth)
    {
        NextCash::String padding;
        for(unsigned int i=0;i<pDepth;i++)
            padding += "  ";

        if(transaction)
        {
            if(matches)
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                  "%sTrans (match) : %s", padding.text(), hash.hex().text());
            else
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                  "%sTrans (no)    : %s", padding.text(), hash.hex().text());
        }
        else if(matches)
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              "%sHash (match) : %s", padding.text(), hash.hex().text());
        else
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              "%sHash (no)    : %s", padding.text(), hash.hex().text());

        if(matches && left != NULL)
        {
            if(matches)
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                  "%s  Left", padding.text(), hash.hex().text());

            left->print(pDepth + 1);

            if(left != right)
            {
                if(matches)
                    NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                      "%s  Right", padding.text(), hash.hex().text());
                right->print(pDepth + 1);
            }
        }
    }

    bool Block::updateOutputsSingleThreaded(Chain *pChain, unsigned int pHeight)
    {
        if(transactions.size() == 0)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "No transactions. At least a coin base is required");
            return false;
        }

        NextCash::Timer addTime(true);
        // Add the transaction outputs from this block to the output pool
        if(!pChain->outputs().add(transactions, pHeight))
            return false;
        addTime.stop();

        NextCash::Timer fullTime;
        unsigned int transactionOffset = 0;
        Transaction::CheckStats stats;
        stats.spentAges.reserve(transactions.size() * 2);
        for(TransactionList::iterator transaction = transactions.begin();
          transaction != transactions.end(); ++transaction)
        {
            fullTime.start();
            if(!(*transaction)->updateOutputs(pChain, pHeight, transactionOffset == 0,
              stats))
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Transaction %d update failed", transactionOffset);
                return false;
            }
            fullTime.stop();
            ++transactionOffset;
        }

        if(stats.spentAges.size() > 0)
        {
            unsigned int totalSpentAge = 0;
            for(std::vector<unsigned int>::iterator spentAge = stats.spentAges.begin();
              spentAge != stats.spentAges.end(); ++spentAge)
                totalSpentAge += *spentAge;
            unsigned int averageSpentAge = totalSpentAge / stats.spentAges.size();
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Average spent age for block %d is %d for %d inputs (Add,%d,Full,%d)", pHeight,
              averageSpentAge, stats.spentAges.size(), addTime.milliseconds(),
              fullTime.milliseconds());
        }

        return true;
    }

    void Block::updateOutputsThreadRun(void *pParameter)
    {
        ProcessThreadData *data = (ProcessThreadData *)pParameter;
        if(data == NULL)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Update outputs block thread parameter is null. Stopping");
            return;
        }

        Transaction *transaction;
        unsigned int offset;
        NextCash::Timer fullTime;
        Transaction::CheckStats stats;
        while(true)
        {
            transaction = data->getNext(offset);
            if(transaction == NULL)
            {
                NextCash::Log::add(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                  "No more transactions to process");
                break;
            }

            fullTime.start();
            if(transaction->updateOutputs(data->chain, data->height, offset == 0, stats))
                data->markComplete(offset, true);
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Transaction %d failed", offset);
                transaction->print(data->chain->forks(), NextCash::Log::WARNING);
                data->markComplete(offset, false);
            }
            fullTime.stop();
        }

        data->statsLock.lock();
        data->fullTime += fullTime.microseconds();
        data->stats += stats;
        data->statsLock.unlock();
    }

    bool Block::updateOutputsMultiThreaded(Chain *pChain, unsigned int pHeight,
      unsigned int pThreadCount)
    {
        mFees = 0;

        if(transactions.size() == 0)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "No transactions. At least a coin base is required");
            return false;
        }

        NextCash::Timer addTime(true);
        // Add the transaction outputs from this block to the output pool
        if(!pChain->outputs().add(transactions, pHeight))
            return false;
        addTime.stop();

        ProcessThreadData threadData(pChain, this, pHeight, transactions.begin(),
          transactions.size());
        NextCash::Thread *threads[pThreadCount];
        int32_t lastReport = getTime();
        unsigned int i;
        NextCash::String threadName;

        // Start threads
        for(i = 0; i < pThreadCount; ++i)
        {
            threadName.writeFormatted("Update Block %d", i);
            threads[i] = new NextCash::Thread(threadName, updateOutputsThreadRun, &threadData);
        }

        NextCash::Thread::sleep(1);

        // Monitor threads
        unsigned int completedCount;
        bool report;
        bool *complete;
        unsigned int checkCount = 0;
        while(threadData.success)
        {
            if(threadData.offset == threadData.count)
            {
                if(++checkCount > 50)
                {
                    checkCount = 0;
                    report = getTime() - lastReport > 10;
                }
                else
                    report = false;
                completedCount = 0;
                complete = threadData.complete;
                for(i = 0; i < threadData.count; ++i, ++complete)
                    if(*complete)
                        ++completedCount;
                    else if(report)
                        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
                          "Update block %d waiting for transaction %d", pHeight, i);

                if(report)
                    lastReport = getTime();

                if(completedCount == threadData.count)
                    break;
            }
            else if(++checkCount > 50)
            {
                checkCount = 0;
                if(getTime() - lastReport > 10)
                {
                    completedCount = 0;
                    complete = threadData.complete;
                    for(i = 0; i < threadData.count; ++i, ++complete)
                        if(*complete)
                            ++completedCount;

                    NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
                      "Update block %d is %2d%% Complete", pHeight,
                      (int)(((float)completedCount / (float)threadData.count) * 100.0f));

                    lastReport = getTime();
                }
            }

            NextCash::Thread::sleep(10);
        }

        // Delete threads
        NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
          "Deleting update block %d threads", pHeight);
        for(i = 0; i < pThreadCount; ++i)
            delete threads[i];

        if(!threadData.success)
            return false;

        for(TransactionList::iterator transaction = transactions.begin() + 1;
          transaction != transactions.end(); ++transaction)
            mFees += (*transaction)->fee();

        // Check that coinbase output amount - fees is correct for block height
        if(-transactions.front()->fee() - mFees > coinBaseAmount(pHeight))
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase outputs are too high");
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase %.08f", bitcoins(-transactions.front()->fee()));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Fees     %.08f", bitcoins(mFees));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block %d Coinbase amount should be %.08f", pHeight,
              bitcoins(coinBaseAmount(pHeight)));
            return false;
        }

        if(threadData.stats.spentAges.size() > 0)
        {
            unsigned int totalSpentAge = 0;
            for(std::vector<unsigned int>::iterator spentAge = threadData.stats.spentAges.begin();
              spentAge != threadData.stats.spentAges.end(); ++spentAge)
                totalSpentAge += *spentAge;
            unsigned int averageSpentAge = totalSpentAge / threadData.stats.spentAges.size();
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Average spent age for block %d is %d for %d inputs (Add,%d,Full,%d)", pHeight,
              averageSpentAge, threadData.stats.spentAges.size(), addTime.milliseconds(),
              threadData.fullTime / 1000L);
        }

        return true;
    }

    bool Block::checkSize(Chain *pChain, unsigned int pHeight)
    {
        if(pChain->forks().cashForkBlockHeight() == pHeight &&
          size() < Forks::HARD_MAX_BLOCK_SIZE)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Cash fork block size must be greater than %d bytes : %d bytes",
              Forks::HARD_MAX_BLOCK_SIZE, size());
            return false;
        }

        if(size() > pChain->forks().blockMaxSize(pHeight))
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block size for height (%d) must not be more than %d bytes : %d",
              pHeight, pChain->forks().blockMaxSize(pHeight), size());
            return false;
        }

        return true;
    }

    bool Block::validate()
    {
        if(transactions.size() == 0)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "No transactions. At least a coin base is required");
            return false;
        }
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_MERKLE_CALC_ID, PROFILER_BLOCK_MERKLE_CALC_NAME), true);
#endif

        // Validate Merkle Hash
        NextCash::Hash calculatedMerkleHash;
        calculateMerkleHash(calculatedMerkleHash);
        if(calculatedMerkleHash != header.merkleHash)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block merkle root hash is invalid");
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Included   : %s", header.merkleHash.hex().text());
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Calculated : %s", calculatedMerkleHash.hex().text());
            return false;
        }

        return true;
    }

    void Block::processThreadRun(void *pParameter)
    {
#ifdef PROFILER_ON
        NextCash::Profiler &profiler = NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_PROCESS_ID, PROFILER_BLOCK_PROCESS_NAME);
        NextCash::Timer profilerTimer(true);
#endif

        ProcessThreadData *data = (ProcessThreadData *)pParameter;
        if(data == NULL)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Process block thread parameter is null. Stopping");
            return;
        }

        Transaction *transaction;
        unsigned int offset;
        NextCash::Timer fullTime, processTime;
        Transaction::CheckStats stats;

        fullTime.start();
        while(true)
        {
            transaction = data->getNext(offset);
            if(transaction == NULL)
            {
                NextCash::Log::add(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
                  "No more transactions to process");
                break;
            }

            processTime.start();
            transaction->check(data->chain, data->block->header.hash(), data->height, offset == 0,
              data->block->header.version, stats);
            if(transaction->isVerified())
                data->markComplete(offset, true);
            else
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Transaction %d failed : %s", offset, transaction->hash().hex().text());
                transaction->print(data->chain->forks(), NextCash::Log::WARNING);
                data->markComplete(offset, false);
            }
            processTime.stop();
        }
        fullTime.stop();

        data->statsLock.lock();
        data->stats += stats;
        data->processTime += processTime.microseconds();
        data->fullTime += fullTime.microseconds();
        data->statsLock.unlock();


#ifdef PROFILER_ON
        profilerTimer.stop();
        profiler.addTime(profilerTimer.microseconds());
#endif
    }

    bool Block::processMultiThreaded(Chain *pChain, unsigned int pHeight,
      unsigned int pThreadCount)
    {
#ifdef PROFILER_ON
        NextCash::Profiler &profiler = NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_PROCESS_ID, PROFILER_BLOCK_PROCESS_NAME);
        profiler.addHits(size());
#endif

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Processing block %d (multi-threaded) (%d trans) (%d KB) : %s", pHeight,
          transactions.size(), size() / 1000, header.hash().hex().text());

        mFees = 0;

        NextCash::Timer addTime(true), elapsed(true);
#ifndef TEST
        // Add the transaction outputs from this block to the output pool
        if(!pChain->outputs().add(transactions, pHeight))
            return false;
#endif
        addTime.stop();

        ProcessThreadData threadData(pChain, this, pHeight, transactions.begin(),
          transactions.size());
        NextCash::Thread *threads[pThreadCount];
        int32_t lastReport = getTime();
        unsigned int i;
        NextCash::String threadName;

        // Start threads
        for(i = 0; i < pThreadCount; ++i)
        {
            threadName.writeFormatted("Process Block %d", i);
            threads[i] = new NextCash::Thread(threadName, processThreadRun, &threadData);
        }

        NextCash::Thread::sleep(1);

        // Monitor threads
        unsigned int completedCount;
        bool report;
        bool *complete;
        unsigned int checkCount = 0;
        while(threadData.success)
        {
            if(threadData.offset == threadData.count)
            {
                if(++checkCount > 50)
                {
                    checkCount = 0;
                    report = getTime() - lastReport > 10;
                }
                else
                    report = false;
                completedCount = 0;
                complete = threadData.complete;
                for(i = 0; i < threadData.count; ++i, ++complete)
                    if(*complete)
                        ++completedCount;
                    else if(report)
                        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
                          "Process block %d waiting for transaction %d", pHeight, i);

                if(report)
                    lastReport = getTime();

                if(completedCount == threadData.count)
                    break;
            }
            else if(++checkCount > 50)
            {
                checkCount = 0;
                if(getTime() - lastReport > 10)
                {
                    completedCount = 0;
                    complete = threadData.complete;
                    for(i = 0; i < threadData.count; ++i, ++complete)
                        if(*complete)
                            ++completedCount;

                    NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
                      "Process block %d is %2d%% Complete", pHeight,
                      (int)(((float)completedCount / (float)threadData.count) * 100.0f));

                    lastReport = getTime();
                }
            }

            NextCash::Thread::sleep(10);
        }

        // Delete threads
        NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
          "Deleting process block %d threads", pHeight);
        for(i = 0; i < pThreadCount; ++i)
            delete threads[i];

        if(!threadData.success)
            return false;

        elapsed.stop();

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Multi threaded block times Threads %d,Add %d,TXO %d,Scr %d,Proc %d,Full %d,Elapsed %d",
          pThreadCount, addTime.milliseconds(),
          threadData.stats.outputsTimer.milliseconds(), threadData.stats.scriptTimer.milliseconds(),
          threadData.processTime / 1000L, threadData.fullTime / 1000L, elapsed.milliseconds());

        if(threadData.stats.spentAges.size() > 0)
        {
            unsigned int totalSpentAge = 0;
            for(std::vector<unsigned int>::iterator spentAge = threadData.stats.spentAges.begin();
              spentAge != threadData.stats.spentAges.end(); ++spentAge)
                totalSpentAge += *spentAge;
            unsigned int averageSpentAge = totalSpentAge / threadData.stats.spentAges.size();
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Average spent age for block %d is %d for %d inputs (%d pulled)", pHeight, averageSpentAge,
              threadData.stats.spentAges.size(), threadData.stats.outputPulls);
        }

        for(TransactionList::iterator transaction = transactions.begin() + 1;
          transaction != transactions.end(); ++transaction)
            mFees += (*transaction)->fee();

        // Check that coinbase output amount - fees is correct for block height
        if(-transactions.front()->fee() - mFees > coinBaseAmount(pHeight))
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase outputs are too high");
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase %.08f", bitcoins(-transactions.front()->fee()));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Fees     %.08f", bitcoins(mFees));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block %d Coinbase amount should be %.08f", pHeight,
              bitcoins(coinBaseAmount(pHeight)));
            return false;
        }

        return true;
    }

    bool Block::processSingleThreaded(Chain *pChain, unsigned int pHeight)
    {
#ifdef PROFILER_ON
        NextCash::Profiler &profiler = NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_PROCESS_ID, PROFILER_BLOCK_PROCESS_NAME);
        profiler.addHits(size() - 1); // One hit (byte) will be added by reference.
        NextCash::ProfilerReference profilerRef(profiler, true);
#endif

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Processing block %d (%d trans) (%d KB) : %s", pHeight,
          transactions.size(), size() / 1000, header.hash().hex().text());

        Transaction::CheckStats stats;
        NextCash::Timer addTime(true), fullTime(true);
#ifndef TEST
        // Add the transaction outputs from this block to the output pool
        if(!pChain->outputs().add(transactions, pHeight))
            return false;
#endif
        addTime.stop();

        // Validate and process transactions
        mFees = 0;
        unsigned int transactionOffset = 0;
        stats.spentAges.reserve(transactions.size() * 2);
        NextCash::Timer processTime;
        for(TransactionList::iterator transaction = transactions.begin();
          transaction != transactions.end(); ++transaction, ++transactionOffset)
        {
            // NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              // "Processing transaction %d", transactionOffset);
            processTime.start();
            (*transaction)->check(pChain, header.hash(), pHeight, transactionOffset == 0,
              header.version, stats);
            if(!(*transaction)->isVerified())
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Transaction %d failed", transactionOffset);
                (*transaction)->print(pChain->forks(), NextCash::Log::WARNING);
                return false;
            }
            processTime.stop();
            if(transactionOffset != 0)
                mFees += (*transaction)->fee();
        }

        fullTime.stop();

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Single threaded block times Threads 1,Add %d,TXO %d,Scr %d,Proc %d,Full %d",
          addTime.milliseconds(), stats.outputsTimer.milliseconds(),
          stats.scriptTimer.milliseconds(), processTime.milliseconds(), fullTime.milliseconds());

        if(stats.spentAges.size() > 0)
        {
            unsigned int totalSpentAge = 0;
            for(std::vector<unsigned int>::iterator spentAge = stats.spentAges.begin();
              spentAge != stats.spentAges.end(); ++spentAge)
                totalSpentAge += *spentAge;
            unsigned int averageSpentAge = totalSpentAge / stats.spentAges.size();
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Average spent age for block %d is %d for %d inputs (%d pulled)", pHeight,
              averageSpentAge, stats.spentAges.size(), stats.outputPulls);
        }

        // Check that coinbase output amount - fees is correct for block height
        if(-transactions.front()->fee() - mFees > coinBaseAmount(pHeight))
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase outputs are too high");
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Coinbase %.08f", bitcoins(-transactions.front()->fee()));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Fees     %.08f", bitcoins(mFees));
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block %d Coinbase amount should be %.08f", pHeight,
              bitcoins(coinBaseAmount(pHeight)));
            return false;
        }

        return true;
    }

    Block *Block::genesis(uint32_t pTargetBits)
    {
        Block *result = new Block();

        result->header.version = 1;
        result->header.previousHash.zeroize();

        if(network() == TESTNET)
        {
            result->header.time = 1296688602;
            result->header.targetBits = pTargetBits;
            result->header.nonce = 414098458;
        }
        else
        {
            result->header.time = 1231006505;
            result->header.targetBits = pTargetBits;
            result->header.nonce = 2083236893;
        }
        result->header.transactionCount = 1;

        Transaction *transaction = new Transaction();
        transaction->version = 1;

        transaction->inputs.emplace_back();
        Input &input = transaction->inputs.back();
        input.script.writeHex("04FFFF001D0104455468652054696D65732030332F4A616E2F32303039204368616E63656C6C6F72206F6E206272696E6B206F66207365636F6E64206261696C6F757420666F722062616E6B73");
        input.script.compact();

        transaction->outputs.emplace_back();
        Output &output = transaction->outputs.back();
        output.amount = 5000000000;
        output.script.writeHex("4104678AFDB0FE5548271967F1A67130B7105CD6A828E03909A67962E0EA1F61DEB649F6BC3F4CEF38C4F35504E51EC112DE5C384DF7BA0B8D578A4C702B6BF11D5FAC");
        output.script.compact();

        transaction->lockTime = 0;
        transaction->calculateHash();

        result->transactions.push_back(transaction);

        result->calculateMerkleHash(result->header.merkleHash);
        result->header.calculateHash();

        return result;
    }

    void Block::finalize()
    {
        //TODO Update total coinbase amount

        header.transactionCount = transactions.size();
        calculateMerkleHash(header.merkleHash);
        header.calculateHash();

        while(!header.hasProofOfWork())
        {
            header.nonce = NextCash::Math::randomLong();
            header.calculateHash();
        }
    }

    class BlockFile
    {
    public:

        static const unsigned int MAX_COUNT = 100; // Maximum count of blocks in one file.

        static unsigned int fileID(unsigned int pHeight) { return pHeight / MAX_COUNT; }
        static unsigned int fileOffset(unsigned int pHeight) { return pHeight - (fileID(pHeight) * MAX_COUNT); }
        static NextCash::String filePathName(unsigned int pID);

        static const unsigned int CACHE_COUNT = 20;
        static NextCash::MutexWithConstantName sCacheLock;
        static BlockFile *sCache[CACHE_COUNT];

        // Return locked header file.
        static BlockFile *get(unsigned int pFileID, bool pWriteAccess, bool pCreate = false);

        // Moves cached header file to the front of the list
        static void moveToFront(unsigned int pOffset);

        static bool exists(unsigned int pID);

        static void save();

        // Cleans up cached data.
        static void clean();

        // Remove a block file
        static bool remove(unsigned int pID);


        BlockFile(unsigned int pID, bool pCreate);
        ~BlockFile() { lock(true); updateCRC(); if(mInputFile != NULL) delete mInputFile; }

        void lock(bool pWriteAccess)
        {
            mLock.lock();
            // if(pWriteAccess)
                // mLock.writeLock();
            // else
                // mLock.readLock();
        }
        void unlock(bool pWriteAccess)
        {
            mLock.unlock();
            // if(pWriteAccess)
                // mLock.writeUnlock();
            // else
                // mLock.readUnlock();
        }

        unsigned int id() const { return mID; }
        bool isValid() const { return mValid; }
        bool isFull() { return itemCount() == MAX_COUNT; }
        unsigned int itemCount() { getLastCount(); return mCount; }
        const NextCash::Hash &lastHash() { getLastCount(); return mLastHash; }

        bool validate(); // Validate CRC

        // Add a block to the file
        bool writeBlock(Block *pBlock);

        // Remove blocks from file above a specific offset in the file
        bool removeBlocksAbove(unsigned int pOffset);

        // Read block at specified offset in file. Return false if the offset is too high.
        bool readTransactions(unsigned int pOffset, TransactionList &pTransactions,
          Time pBlockTime, NextCash::stream_size *pDataSize = NULL);

        bool readOutput(unsigned int pBlockOffset, unsigned int pTransactionOffset,
          unsigned int pOutputIndex, NextCash::Hash &pTransactionID, Output &pOutput);

    private:

        /* File format
         *   Start string
         *   CRC32 of data after CRC in file
         *   MAX_COUNT Index entries (32 byte block hash, 4 byte offset into file of block data)
         *   Data - Transactions for blocks
         */
        static const unsigned int CRC_OFFSET = 8; // After start string
        static const unsigned int HEADER_START_OFFSET = 12;
        static const unsigned int HEADER_ITEM_SIZE = 36; // 32 byte hash, 4 byte data offset
        static const unsigned int DATA_START_OFFSET = HEADER_START_OFFSET +
          (MAX_COUNT * HEADER_ITEM_SIZE);
        static constexpr const char *START_STRING = "NCBLKS01";
        static const unsigned int INVALID_COUNT = 0xffffffff;

        static NextCash::String sFilePath;

        // Open and validate a file stream for reading
        bool openFile(bool pCreate = false);

        void updateCRC();

        unsigned int mID;
        NextCash::MutexWithConstantName mLock;
        NextCash::FileInputStream *mInputFile;
        NextCash::String mFilePathName;
        bool mValid;
        bool mModified;

        void getLastCount();
        unsigned int mCount;
        NextCash::Hash mLastHash;

        BlockFile(BlockFile &pCopy);
        BlockFile &operator = (BlockFile &pRight);

    };

    NextCash::String BlockFile::sFilePath;
    NextCash::MutexWithConstantName BlockFile::sCacheLock("BlockFileCache");
    BlockFile *BlockFile::sCache[CACHE_COUNT] = { NULL, NULL, NULL, NULL, NULL };

    void BlockFile::moveToFront(unsigned int pOffset)
    {
        static BlockFile *swap[CACHE_COUNT] = { NULL, NULL, NULL, NULL, NULL };

        if(pOffset == 0)
            return;

        unsigned int next = 0;
        swap[next++] = sCache[pOffset];
        for(unsigned int j = 0; j < (int)CACHE_COUNT; ++j)
            if(j != pOffset)
                swap[next++] = sCache[j];

        // Swap back
        for(unsigned int j = 0; j < CACHE_COUNT; ++j)
            sCache[j] = swap[j];
    }

    bool BlockFile::exists(unsigned int pFileID)
    {
        return NextCash::fileExists(BlockFile::filePathName(pFileID));
    }

    BlockFile *BlockFile::get(unsigned int pFileID, bool pWriteAccess, bool pCreate)
    {
        sCacheLock.lock();

        // Check if the file is already open
        for(unsigned int i = 0; i < CACHE_COUNT; ++i)
            if(sCache[i] != NULL && sCache[i]->mID == pFileID)
            {
                BlockFile *result = sCache[i];
                result->lock(pWriteAccess);
                moveToFront(i);
                sCacheLock.unlock();
                return result;
            }

        // Open file
        BlockFile *result = new BlockFile(pFileID, pCreate);
        if(!result->isValid())
        {
            delete result;
            sCacheLock.unlock();
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x failed to open.", pFileID);
            return NULL;
        }

        result->lock(pWriteAccess);

        for(unsigned int i = 0; i < CACHE_COUNT; ++i)
            if(sCache[i] == NULL)
            {
                sCache[i] = result;
                moveToFront(i);
                sCacheLock.unlock();
                return result;
            }

        // Replace the last file
        delete sCache[CACHE_COUNT - 1];
        sCache[CACHE_COUNT - 1] = result;
        moveToFront(CACHE_COUNT-1);
        sCacheLock.unlock();
        return result;
    }

    bool BlockFile::remove(unsigned int pFileID)
    {
        // Remove from cache.
        sCacheLock.lock();

        // Check if the file is already open.
        bool found = false;
        for(unsigned int i = 0; i < CACHE_COUNT; ++i)
            if(sCache[i] != NULL && sCache[i]->mID == pFileID)
            {
                delete sCache[i];
                sCache[i] = NULL;
                found = true;
                break;
            }

        if(found)
        {
            // Push any files after up a slot.
            for(unsigned int i = 0; i < CACHE_COUNT - 1; ++i)
                if(sCache[i] == NULL)
                {
                    sCache[i] = sCache[i+1];
                    sCache[i+1] = NULL;
                }
        }

        if(NextCash::removeFile(filePathName(pFileID)))
        {
            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
              "Removed block file %08x", pFileID);
            sCacheLock.unlock();
            return true;
        }
        else
            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_BLOCK_LOG_NAME,
              "Failed to remove block file %08x", pFileID);

        sCacheLock.unlock();
        return false;
    }

    void BlockFile::save()
    {
        sCacheLock.lock();
        for(int i = CACHE_COUNT-1; i >= 0; --i)
            if(sCache[i] != NULL)
            {
                sCache[i]->lock(true);
                sCache[i]->updateCRC();
                sCache[i]->unlock(true);
            }
        sCacheLock.unlock();
    }

    void BlockFile::clean()
    {
        sCacheLock.lock();
        for(int i = CACHE_COUNT-1; i >= 0; --i)
            if(sCache[i] != NULL)
            {
                delete sCache[i];
                sCache[i] = NULL;
            }
        sCacheLock.unlock();

        sFilePath.clear();
    }

    void Block::save()
    {
        BlockFile::save();
    }

    void Block::clean()
    {
        BlockFile::clean();
    }

    NextCash::String BlockFile::filePathName(unsigned int pID)
    {
        if(!sFilePath)
        {
            // Build path
            sFilePath = Info::instance().path();
            sFilePath.pathAppend("blocks");
            NextCash::createDirectory(sFilePath);
        }

        // Build path
        NextCash::String result;
        result.writeFormatted("%s%s%08x", sFilePath.text(), NextCash::PATH_SEPARATOR, pID);
        return result;
    }

    BlockFile::BlockFile(unsigned int pID, bool pCreate) : mLock("BlockFile")
    {
        mValid = true;
        mFilePathName = filePathName(pID);
        mInputFile = NULL;
        mID = pID;
        mModified = false;
        mCount = INVALID_COUNT;

        if(!openFile(pCreate))
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              "Failed to open block file : %s", mFilePathName.text());
            mValid = false;
            return;
        }

        // Read start string
        NextCash::String startString = mInputFile->readString(8);

        // Check start string
        if(startString != START_STRING)
        {
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x missing start string", mID);
            mValid = false;
            return;
        }
    }

    bool BlockFile::openFile(bool pCreate)
    {
        if(mInputFile != NULL && mInputFile->isValid())
            return true;

        if(mInputFile != NULL)
            delete mInputFile;

        mInputFile = new NextCash::FileInputStream(mFilePathName);
        mInputFile->setInputEndian(NextCash::Endian::LITTLE);
        mInputFile->setReadOffset(0);

        if(mInputFile->isValid())
            return true;
        else if(!pCreate)
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x not found.", mID);
            return false;
        }

        // Create new file
        delete mInputFile;
        mInputFile = NULL;

        NextCash::FileOutputStream *outputFile = new NextCash::FileOutputStream(mFilePathName,
          true);
        outputFile->setOutputEndian(NextCash::Endian::LITTLE);

        if(!outputFile->isValid())
        {
            delete outputFile;
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x failed to open.", mID);
            return false;
        }

        // Write start string
        outputFile->writeString(START_STRING);

        // Write empty CRC
        outputFile->writeUnsignedInt(0);

        // Write empty index entries
        NextCash::Digest digest(NextCash::Digest::CRC32);
        digest.setOutputEndian(NextCash::Endian::LITTLE);
        NextCash::Hash zeroHash(BLOCK_HASH_SIZE);
        for(unsigned int i = 0; i < MAX_COUNT; ++i)
        {
            zeroHash.write(outputFile); // Block hash
            outputFile->writeUnsignedInt(0); // Data offset

            // For digest
            zeroHash.write(&digest);
            digest.writeUnsignedInt(0);
        }

        // Get initial CRC
        NextCash::Buffer crcBuffer;
        crcBuffer.setEndian(NextCash::Endian::LITTLE);
        digest.getResult(&crcBuffer);
        uint32_t crc = crcBuffer.readUnsignedInt();

        // Write CRC
        outputFile->setWriteOffset(CRC_OFFSET);
        outputFile->writeUnsignedInt(crc);

        // Close file
        delete outputFile;

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Block file %08x created with CRC : %08x", mID, crc);

        // Re-open file
        mInputFile = new NextCash::FileInputStream(mFilePathName);
        mInputFile->setInputEndian(NextCash::Endian::LITTLE);
        mInputFile->setReadOffset(0);

        return mInputFile->isValid();
    }

    void BlockFile::updateCRC()
    {
        if(!mModified || !mValid)
            return;

        if(!openFile())
        {
            mValid = false;
            return;
        }

        // Calculate new CRC
        NextCash::Digest digest(NextCash::Digest::CRC32);
        digest.setOutputEndian(NextCash::Endian::LITTLE);

        // Read file into digest
        mInputFile->setReadOffset(HEADER_START_OFFSET);
        digest.writeStream(mInputFile, mInputFile->remaining());

        // Close input file
        delete mInputFile;
        mInputFile = NULL;

        // Get CRC result
        NextCash::Buffer crcBuffer;
        crcBuffer.setEndian(NextCash::Endian::LITTLE);
        digest.getResult(&crcBuffer);
        uint32_t crc = crcBuffer.readUnsignedInt();

        // Open output file
        NextCash::FileOutputStream *outputFile = new NextCash::FileOutputStream(mFilePathName);

        // Write CRC to file
        outputFile->setOutputEndian(NextCash::Endian::LITTLE);
        outputFile->setWriteOffset(CRC_OFFSET);
        outputFile->writeUnsignedInt(crc);

        // Close output file
        delete outputFile;
        mModified = false;

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Block file %08x CRC updated : 0x%08x", mID, crc);
    }

    bool BlockFile::validate()
    {
        // Read CRC
        mInputFile->setReadOffset(CRC_OFFSET);
        uint32_t crc = mInputFile->readUnsignedInt();

        // Calculate CRC
        NextCash::Digest digest(NextCash::Digest::CRC32);
        digest.setOutputEndian(NextCash::Endian::LITTLE);
        digest.writeStream(mInputFile, mInputFile->remaining());

        // Get Calculated CRC
        NextCash::Buffer crcBuffer;
        crcBuffer.setEndian(NextCash::Endian::LITTLE);
        digest.getResult(&crcBuffer);
        uint32_t calculatedCRC = crcBuffer.readUnsignedInt();

        // Check CRC
        if(crc == calculatedCRC)
            return true;

        // Attempt to verify the data in the file.
        mValid = true;

        NextCash::Hash hash(BLOCK_HASH_SIZE);
        uint32_t dataOffset;
        Block block;
        Transaction *transaction;
        uint32_t transactionCount;
        unsigned int lastGoodCount = 0;
        NextCash::stream_size lastGoodOffset = 0;
        unsigned int previousCount = 0;
        bool fail;

        // Find current block count.
        mInputFile->setReadOffset(HEADER_START_OFFSET);
        while(previousCount < MAX_COUNT)
        {
            if(!hash.read(mInputFile))
                break;

            if(hash.isZero())
                break;

            mInputFile->skip(4);
            ++previousCount;
        }

        while(lastGoodCount < MAX_COUNT)
        {
            mInputFile->setReadOffset(HEADER_START_OFFSET + (lastGoodCount * HEADER_ITEM_SIZE));
            if(!hash.read(mInputFile))
                break;

            if(hash.isZero())
                break;

            if(mInputFile->remaining() < 4)
                break;
            dataOffset = mInputFile->readUnsignedInt();

            if(dataOffset + 4 > mInputFile->length())
                break;

            if(!Header::getHeader((mID * MAX_COUNT) + lastGoodCount, block.header))
                break;

            mInputFile->setReadOffset(dataOffset);

            if(mInputFile->remaining() < 4)
                break;
            transactionCount = mInputFile->readUnsignedInt();

            fail = false;
            block.transactions.reserve(transactionCount);
            for(unsigned int i = 0; i < transactionCount; ++i)
            {
                transaction = new Transaction();
                if(transaction->read(mInputFile))
                    block.transactions.emplace_back(transaction);
                else
                {
                    delete transaction;
                    fail = true;
                    break;
                }
            }

            if(fail)
                break;

            if(!block.validate())
                break;

            block.clear();
            lastGoodOffset = mInputFile->readOffset();
            ++lastGoodCount;
        }

        if(lastGoodOffset == 0)
        {
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x has no good blocks", mID);
            return false;
        }

        NextCash::stream_size truncateSize = mInputFile->length() - lastGoodOffset;
        if(truncateSize != 0)
        {
            // Truncate end of file.
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x reverting from count of %d to %d", mID, previousCount,
              lastGoodCount);

            NextCash::String swapFilePathName = mFilePathName + ".swap";
            NextCash::FileOutputStream *swapFile = new NextCash::FileOutputStream(swapFilePathName,
              true);

            if(!swapFile->isValid())
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Failed to repair block file %08x. Failed to open swap file", mID);
                delete swapFile;
                return false;
            }

            // Write start string
            swapFile->writeString(START_STRING);

            // Write empty CRC
            swapFile->writeUnsignedInt(0);

            mInputFile->setReadOffset(HEADER_START_OFFSET);

            // Transfer block to swap file
            for(unsigned int i = 0; i < lastGoodCount; ++i)
            {
                if(!hash.read(mInputFile))
                    return false;
                hash.write(swapFile);
                swapFile->writeUnsignedInt(mInputFile->readUnsignedInt());
            }

            mLastHash = hash;

            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x new last hash : %s", mID, mLastHash.hex().text());

            // Write the rest of the block as empty
            hash.zeroize();
            for(unsigned int i = lastGoodCount; i < MAX_COUNT; ++i)
            {
                hash.write(swapFile);
                swapFile->writeUnsignedInt(0);
            }

            // Copy block data to swap file
            mInputFile->setReadOffset(DATA_START_OFFSET);
            swapFile->writeStream(mInputFile, lastGoodOffset - swapFile->writeOffset());

            delete mInputFile;
            mInputFile = NULL;
            delete swapFile;

            if(!NextCash::renameFile(swapFilePathName, mFilePathName))
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
                  "Failed to repair block file %08x. Failed to rename swap file", mID);
                return false;
            }
        }

        NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
          "Repaired block file %08x. Truncated %d blocks", mID, previousCount - lastGoodCount);
        mModified = true;
        updateCRC();
        return true;
    }

    void BlockFile::getLastCount()
    {
        if(mCount != INVALID_COUNT)
            return;

        mCount = 0;
        mLastHash.clear();

        if(!openFile())
        {
            mValid = false;
            return;
        }

        // Go to the last data offset in the header
        mInputFile->setReadOffset(HEADER_START_OFFSET + ((MAX_COUNT - 1) * HEADER_ITEM_SIZE) +
          BLOCK_HASH_SIZE);

        // Check each data offset until it is not empty
        for(mCount = MAX_COUNT; mCount > 0; --mCount)
        {
            if(mInputFile->readUnsignedInt() != 0)
            {
                // Back up to hash for this data offset
                mInputFile->setReadOffset(mInputFile->readOffset() - HEADER_ITEM_SIZE);
                if(!mLastHash.read(mInputFile, BLOCK_HASH_SIZE))
                {
                    mLastHash.clear();
                    mValid = false;
                }
                break;
            }
            else // Back up to previous data offset
                mInputFile->setReadOffset(mInputFile->readOffset() - HEADER_ITEM_SIZE - 4);
        }
    }

    bool BlockFile::writeBlock(Block *pBlock)
    {
        if(!openFile())
            return false;

        unsigned int count = itemCount();

        if(count == MAX_COUNT)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x is already full", mID);
            return false;
        }

        // New blocks are appended to the file
        NextCash::stream_size nextBlockOffset = mInputFile->length();

        if(mInputFile != NULL)
            delete mInputFile;
        mInputFile = NULL;

        NextCash::FileOutputStream *outputFile = new NextCash::FileOutputStream(mFilePathName);
        outputFile->setOutputEndian(NextCash::Endian::LITTLE);
        if(!outputFile->isValid())
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x output file failed to open", mID);
            delete outputFile;
            return false;
        }

        // Write hash and offset to file
        outputFile->setWriteOffset(HEADER_START_OFFSET + (count * HEADER_ITEM_SIZE));
        pBlock->header.hash().write(outputFile);
        outputFile->writeUnsignedInt(nextBlockOffset);

        // Write block data at end of file
        outputFile->setWriteOffset(nextBlockOffset);

        // Transaction count (4 bytes)
        outputFile->writeUnsignedInt(pBlock->transactions.size());

        // Transactions
        for(TransactionList::iterator trans = pBlock->transactions.begin();
          trans != pBlock->transactions.end(); ++trans)
            (*trans)->write(outputFile);

        delete outputFile;

        mLastHash = pBlock->header.hash();
        ++mCount;
        mModified = true;

        // Update CRC when the file is full.
        if(mCount == MAX_COUNT)
            updateCRC();
        return true;
    }

    bool Block::add(unsigned int pHeight, Block *pBlock)
    {
        BlockFile *file = BlockFile::get(BlockFile::fileID(pHeight), true, true);
        if(file == NULL)
            return false;

        if(file->itemCount() != BlockFile::fileOffset(pHeight))
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x add block failed : Invalid block height : file %d / added %d",
              (file->id() * BlockFile::MAX_COUNT) + file->itemCount(), pHeight);
            file->unlock(true);
            return false;
        }

        if(pHeight != 0)
        {
            // Check previous hash
            if(file->itemCount() == 0)
            {
                // First block in file. Verify last hash of previous file.
                BlockFile *previousFile = BlockFile::get(BlockFile::fileID(pHeight) - 1, true);
                if(previousFile == NULL)
                {
                    file->unlock(true);
                    return false;
                }

                if(previousFile->lastHash() != pBlock->header.previousHash)
                {
                    NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                      "Block file %08x add block (%d) failed : Invalid previous hash : %s",
                      file->id(), pHeight, pBlock->header.previousHash.hex().text());
                    NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                      "Does not match last hash of previous block file : %s",
                      previousFile->lastHash().hex().text());
                    file->unlock(true);
                    previousFile->unlock(true);
                    return false;
                }

                previousFile->unlock(true);
            }
            else if(file->lastHash() != pBlock->header.previousHash)
            {
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Block file %08x add block (%d) failed : Invalid previous hash : %s",
                  file->id(), pHeight, pBlock->header.previousHash.hex().text());
                NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
                  "Does not match last hash of block file : %s", file->lastHash().hex().text());
                file->unlock(true);
                return false;
            }
        }

        bool success = file->writeBlock(pBlock);
        file->unlock(true);
        return success;
    }

    bool BlockFile::removeBlocksAbove(unsigned int pOffset)
    {
        if(!openFile())
            return false;

        unsigned int count = itemCount();
        if(count <= pOffset || pOffset >= (MAX_COUNT - 1))
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x offset not above %d", mID, pOffset);
            return false;
        }

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Block file %08x reverting to count of %d", mID, pOffset);

        NextCash::String swapFilePathName = mFilePathName + ".swap";
        NextCash::FileOutputStream *swapFile = new NextCash::FileOutputStream(swapFilePathName,
          true);

        if(!swapFile->isValid())
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x swap output file failed to open", mID);
            delete swapFile;
            return false;
        }

        // Write start string
        swapFile->writeString(START_STRING);

        // Write empty CRC
        swapFile->writeUnsignedInt(0);

        mInputFile->setReadOffset(HEADER_START_OFFSET);
        NextCash::Hash hash(BLOCK_HASH_SIZE);

        // Transafer block to swap file
        for(unsigned int i = 0; i <= pOffset; ++i)
        {
            if(!hash.read(mInputFile))
                return false;
            hash.write(swapFile);
            swapFile->writeUnsignedInt(mInputFile->readUnsignedInt());
        }

        mLastHash = hash;

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Block file %08x new last hash : %s", mID, mLastHash.hex().text());

        // Get data offset of first block to be removed
        if(!hash.read(mInputFile))
            return false;
        NextCash::stream_size newFileSize = mInputFile->readUnsignedInt();

        // Write the rest of the block as empty
        hash.zeroize();
        for(unsigned int i = pOffset + 1; i < MAX_COUNT; ++i)
        {
            hash.write(swapFile);
            swapFile->writeUnsignedInt(0);
        }

        // Copy block data to swap file
        mInputFile->setReadOffset(DATA_START_OFFSET);
        swapFile->writeStream(mInputFile, newFileSize - swapFile->writeOffset());

        delete mInputFile;
        mInputFile = NULL;
        delete swapFile;

        mCount = pOffset + 1;
        mModified = true;

        if(!NextCash::renameFile(swapFilePathName, mFilePathName))
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_BLOCK_LOG_NAME,
              "Block file %08x failed to rename swap file", mID);
            return false;
        }

        updateCRC();
        return true;
    }

    bool Block::revertToHeight(unsigned int pHeight)
    {
        unsigned int fileID = BlockFile::fileID(pHeight);
        unsigned int fileOffset = BlockFile::fileOffset(pHeight);

        // Truncate latest file
        if(fileOffset != BlockFile::MAX_COUNT - 1)
        {
            BlockFile *file = BlockFile::get(fileID, true);
            if(file == NULL)
                return false;

            file->removeBlocksAbove(fileOffset);
            file->unlock(true);
        }
        ++fileID;

        // Remove any files after that
        while(true)
        {
            if(!BlockFile::remove(fileID))
                return !BlockFile::exists(fileID);

            ++fileID;
        }
    }

    bool BlockFile::readTransactions(unsigned int pOffset, TransactionList &pTransactions,
      Time pBlockTime, NextCash::stream_size *pDataSize)
    {
        if(!openFile())
        {
            mValid = false;
            return false;
        }

        // Go to location in header where the data offset to the block is
        mInputFile->setReadOffset(HEADER_START_OFFSET + (pOffset * HEADER_ITEM_SIZE) +
          BLOCK_HASH_SIZE);

        NextCash::stream_size offset = mInputFile->readUnsignedInt();
        if(offset == 0)
            return false;

        Transaction *transaction;
        mInputFile->setReadOffset(offset);
        uint32_t transactionCount = mInputFile->readUnsignedInt();
        if(transactionCount > MAX_BLOCK_TRANSACTIONS)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Failed to read transactions from block file 0x%08x. Too many : %d", mID,
              transactionCount);
            return false;
        }

        pTransactions.reserve(transactionCount);
        for(unsigned int i = 0; i < transactionCount; ++i)
        {
            transaction = new Transaction();
            transaction->setTime(pBlockTime);
            if(transaction->read(mInputFile))
                pTransactions.emplace_back(transaction);
            else
            {
                delete transaction;
                return false;
            }
        }

        if(pDataSize != NULL)
            *pDataSize = mInputFile->readOffset() - offset;

        return true;
    }

    Block *Block::getBlock(unsigned int pHeight)
    {
#ifdef PROFILER_ON
        NextCash::Profiler &profiler = NextCash::getProfiler(PROFILER_SET,
          PROFILER_BLOCK_GET_ID, PROFILER_BLOCK_GET_NAME);
        NextCash::ProfilerReference profilerRef(profiler, true);
#endif
        Block *result = new Block();

        if(!Header::getHeader(pHeight, result->header))
        {
            delete result;
            return NULL;
        }

        BlockFile *file = BlockFile::get(BlockFile::fileID(pHeight), false);
        if(file == NULL)
        {
            delete result;
            return NULL;
        }

        NextCash::stream_size dataSize = 0;
        if(file->readTransactions(BlockFile::fileOffset(pHeight), result->transactions,
          result->header.time, &dataSize))
        {
            result->header.transactionCount = result->transactions.size();
            result->setSize(80 + compactIntegerSize(result->header.transactionCount) + dataSize);
        }
        else
        {
            delete result;
            result = NULL;
        }
        file->unlock(false);

#ifdef PROFILER_ON
        if(result != NULL)
            profiler.addHits(result->size() - 1); // One hit (byte) will be added by reference.
#endif
        return result;
    }

    bool BlockFile::readOutput(unsigned int pBlockOffset, unsigned int pTransactionOffset,
      unsigned int pOutputIndex, NextCash::Hash &pTransactionID, Output &pOutput)
    {
        if(!openFile())
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Failed to read output. Block file 0x%08x couldn't be opened.", mID);
            mValid = false;
            return false;
        }

        // Go to location in header where the data offset to the block is
        mInputFile->setReadOffset(HEADER_START_OFFSET + (pBlockOffset * HEADER_ITEM_SIZE) + BLOCK_HASH_SIZE);

        unsigned int offset = mInputFile->readUnsignedInt();
        if(offset == 0)
            return false;

        mInputFile->setReadOffset(offset); // Go to block data

        uint32_t transactionCount = mInputFile->readUnsignedInt();
        if(transactionCount <= pTransactionOffset)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
              "Block at offset %d doesn't have enough transactions %d/%d. Block file 0x%08x couldn't be opened.",
              pBlockOffset, pTransactionOffset, transactionCount, mID);
            return false;
        }

        for(int i=0;i<(int)pTransactionOffset;++i)
            if(!Transaction::skip(mInputFile))
                return false;

        return Transaction::readOutput(mInputFile, pOutputIndex, pTransactionID, pOutput);
    }

    bool Block::getOutput(unsigned int pHeight, unsigned int pTransactionOffset,
      unsigned int pOutputIndex, NextCash::Hash &pTransactionID, Output &pOutput)
    {
        BlockFile *file = BlockFile::get(BlockFile::fileID(pHeight), false);
        if(file == NULL)
            return false;

        unsigned int offset = BlockFile::fileOffset(pHeight);

        bool success = file->readOutput(offset, pTransactionOffset, pOutputIndex, pTransactionID,
          pOutput);
        file->unlock(false);
        return success;
    }

    unsigned int Block::totalCount()
    {
        unsigned int result = 0;
        unsigned int fileID = 0;
        while(BlockFile::exists(fileID))
        {
            result += BlockFile::MAX_COUNT;
            ++fileID;
        }

        if(fileID > 0)
        {
            // Adjust for last file not being full.
            --fileID;
            result -= BlockFile::MAX_COUNT;

            BlockFile *file = BlockFile::get(fileID, false);
            if(file != NULL)
            {
                result += file->itemCount();
                file->unlock(false);
            }
        }

        return result;
    }

    unsigned int Block::validate(bool &pAbort)
    {
        NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_BLOCK_LOG_NAME,
          "Validating block files");

        unsigned int result = 0;
        unsigned int fileID = 0;
        BlockFile *file;

        // Find top file ID.
        while(!pAbort && BlockFile::exists(fileID))
            fileID += 100;

        if(pAbort)
            return 0;

        while(!pAbort && fileID > 0 && !BlockFile::exists(fileID))
            --fileID;

        if(pAbort)
            return 0;

        result = fileID * BlockFile::MAX_COUNT;

        // Adjust for last file not being full.
        while(!pAbort)
        {
            file = BlockFile::get(fileID, false);
            if(file == NULL)
            {
                BlockFile::remove(fileID);
                if(fileID == 0)
                    break;
                --fileID;
                result -= BlockFile::MAX_COUNT;
            }
            else if(file->validate())
            {
                result += file->itemCount();
                file->unlock(false);
                break;
            }
            else
            {
                file->unlock(false);
                BlockFile::remove(fileID);
                if(fileID == 0)
                    break;
                --fileID;
                result -= BlockFile::MAX_COUNT;
            }
        }

        return result;
    }

    void BlockStat::set(BlockReference &pBlock, unsigned int pHeight)
    {
        hash = pBlock->header.hash();
        time = pBlock->header.time;
        size = pBlock->size();
        transactionCount = pBlock->transactions.size();
        inputCount = 0;
        outputCount = 0;
        fees = 0UL;
        amount = 0UL;

        if(pBlock->transactions.size() == 0)
            return;

        fees = pBlock->actualCoinbaseAmount() - coinBaseAmount(pHeight);

        for(TransactionList::iterator trans = pBlock->transactions.begin() + 1;
          trans != pBlock->transactions.end(); ++trans)
        {
            inputCount += (*trans)->inputs.size();
            outputCount += (*trans)->outputs.size();
            for(std::vector<Output>::iterator output = (*trans)->outputs.begin();
              output != (*trans)->outputs.end(); ++output)
                amount += output->amount;
        }
    }

    void BlockStat::write(NextCash::OutputStream *pStream) const
    {
        hash.write(pStream);
        pStream->writeUnsignedInt(time);
        pStream->writeUnsignedLong(size);
        pStream->writeUnsignedInt(transactionCount);
        pStream->writeUnsignedInt(inputCount);
        pStream->writeUnsignedInt(outputCount);
        pStream->writeUnsignedLong(fees);
        pStream->writeUnsignedLong(amount);
    }

    bool BlockStat::read(NextCash::InputStream *pStream)
    {
        if(pStream->remaining() < DATA_SIZE)
            return false;
        if(!hash.read(pStream, BLOCK_HASH_SIZE))
            return false;
        time = pStream->readUnsignedInt();
        size = pStream->readUnsignedLong();
        transactionCount = pStream->readUnsignedInt();
        inputCount = pStream->readUnsignedInt();
        outputCount = pStream->readUnsignedInt();
        fees = pStream->readUnsignedLong();
        amount = pStream->readUnsignedLong();
        return true;
    }
}
