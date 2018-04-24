/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                       *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.com>                                    *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "requests.hpp"

#include "log.hpp"
#include "hash.hpp"
#include "digest.hpp"

#include "key.hpp"
#include "info.hpp"

#include <algorithm>


namespace BitCoin
{
    unsigned int RequestChannel::mNextID = 256;

    RequestChannel::RequestChannel(NextCash::Network::Connection *pConnection, Chain *pChain) : mID(mNextID++), mConnectionMutex("Request Connection")
    {
        mThread = NULL;
        mConnection = NULL;
        mStop = false;
        mStopped = false;
        mAuthenticated = false;
        mChain = pChain;
        mName.writeFormatted("Request [%d]", mID);

        mLastReceiveTime = getTime();
        mConnectedTime = getTime();

        // Verify connection
        mConnectionMutex.lock();
        mConnection = pConnection;
        mConnectionMutex.unlock();
        NextCash::Log::addFormatted(NextCash::Log::INFO, mName, "Requests Connection %s : %d",
          mConnection->ipv6Address(), mConnection->port());

        // Start thread
        mThread = new NextCash::Thread("Request", run, this);
        NextCash::Thread::sleep(100); // Give the thread a chance to initialize
    }

    RequestChannel::~RequestChannel()
    {
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
          "Disconnecting %s", mConnection->ipv6Address());

        requestStop();
        if(mThread != NULL)
            delete mThread;
        mConnectionMutex.lock();
        if(mConnection != NULL)
            delete mConnection;
        mConnectionMutex.unlock();
    }

    void RequestChannel::run()
    {
        RequestChannel *requestChannel = (RequestChannel *)NextCash::Thread::getParameter();
        if(requestChannel == NULL)
        {
            NextCash::Log::add(NextCash::Log::ERROR, "Request", "Thread parameter is null. Stopping");
            return;
        }

        if(requestChannel->mStop)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, requestChannel->mName,
              "Request channel stopped before thread started");
            requestChannel->mStopped = true;
            return;
        }

        while(!requestChannel->mStop)
        {
            requestChannel->process();
            if(requestChannel->mStop)
                break;
            NextCash::Thread::sleep(100);
        }

        requestChannel->mStopped = true;
    }

    void RequestChannel::requestStop()
    {
        if(mThread == NULL)
            return;
        mConnectionMutex.lock();
        if(mConnection != NULL && mConnection->isOpen())
        {
            NextCash::Buffer closeBuffer;
            closeBuffer.writeString("clse:");
            mConnection->send(&closeBuffer);
        }
        mConnectionMutex.unlock();
        mStop = true;
    }

    void RequestChannel::process()
    {
        mConnectionMutex.lock();
        if(mConnection == NULL)
        {
            mConnectionMutex.unlock();
            return;
        }

        if(!mConnection->isOpen())
        {
            mConnectionMutex.unlock();
            requestStop();
            return;
        }
        mConnection->receive(&mReceiveBuffer);
        mConnectionMutex.unlock();

        if(getTime() - mLastReceiveTime > 120)
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, mName,
              "Timed out waiting for message");
            requestStop();
            return;
        }

        if(!mAuthenticated)
        {
            if(getTime() - mConnectedTime > 60)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "Timed out waiting for authentication");
                requestStop();
                return;
            }

            // Check for auth command
            mReceiveBuffer.setReadOffset(0);
            if(mReceiveBuffer.remaining() < 5)
                return;

            NextCash::String authString = mReceiveBuffer.readString(5);
            if(authString != "auth:")
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Invalid authentication command : %s", authString.text());
                requestStop();
                return;
            }

            // Read signature
            if(!mReceiveBuffer.remaining())
                return;

            if(mReceiveBuffer.readByte() != 0x30)
            {
                mReceiveBuffer.setReadOffset(mReceiveBuffer.readOffset() - 1);
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Signature doesn't start with compound header byte : %02x", mReceiveBuffer.readByte());
                requestStop();
                return;
            }

            unsigned int sigLength = mReceiveBuffer.readByte() + 3; // Plus header byte, length byte, hash type byte
            mReceiveBuffer.setReadOffset(mReceiveBuffer.readOffset() - 2);
            Signature signature;
            if(!signature.read(&mReceiveBuffer, sigLength, true))
                return;

            // Generate hashes to check
            uint32_t value = getTime();
            value -= value % 10;
            value -= 30;
            NextCash::Hash hashes[5];
            NextCash::Digest digest(NextCash::Digest::SHA256);
            for(int i=0;i<5;++i)
            {
                digest.initialize();
                digest.writeUnsignedInt(value);
                digest.getResult(&hashes[i]);
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  "Auth hash %d : %s", value, hashes[i].hex().text());
                value += 10;
            }

            // Open public keys file
            NextCash::String keysFilePathName = Info::instance().path();
            keysFilePathName.pathAppend("keys");
            NextCash::FileInputStream keysFile(keysFilePathName);

            if(!keysFile.isValid())
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Failed to open keys file : %s", keysFilePathName.text());
                requestStop();
                return;
            }

            // Check signature against authorized public keys
            Key publicKey;
            NextCash::String keyText, keyName;
            NextCash::Buffer keyBuffer, keyData;
            char nextChar;
            unsigned int authorizedCount = 0;
            NextCash::Hash validHash;
            while(keysFile.remaining())
            {
                keyName.clear();
                keyText.clear();
                while(keysFile.remaining())
                {
                    nextChar = keysFile.readByte();
                    if(nextChar == '\r' || nextChar == '\n')
                    {
                        keysFile.setReadOffset(keysFile.readOffset() - 1);
                        break;
                    }
                    else if(nextChar == ' ')
                        break;

                    keyText += nextChar;
                }

                if(nextChar == ' ')
                    while(keysFile.remaining())
                    {
                        nextChar = keysFile.readByte();
                        if(nextChar == '\r' || nextChar == '\n')
                        {
                            keysFile.setReadOffset(keysFile.readOffset() - 1);
                            break;
                        }

                        keyName += nextChar;
                    }

                keyBuffer.clear();
                keyBuffer.writeHex(keyText);

                if(!publicKey.readPublic(&keyBuffer))
                    break;
                keyBuffer.clear();
                publicKey.writePublic(&keyBuffer, false);
                NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
                  "Checking public key %s : %s", keyName.text(), keyBuffer.readHexString(keyBuffer.remaining()).text());
                ++authorizedCount;

                for(int i=0;i<5;++i)
                    if(publicKey.verify(signature, hashes[i]))
                    {
                        validHash = hashes[i];
                        mAuthenticated = true;
                        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                          "Connection authorized : %s", keyName.text());
                        break;
                    }

                if(mAuthenticated)
                    break;

                if(!keysFile.remaining())
                    break;

                // Parse end of line character(s)
                nextChar = keysFile.readByte();
                if(nextChar == '\r')
                {
                    if(keysFile.readByte() != '\n')
                        break;
                }
                else if(nextChar != '\n')
                    break;
            }

            if(!mAuthenticated)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Failed to authenticate : %d authorized users", authorizedCount);
                requestStop();
                return;
            }

            // Send signature back to prove identity
            NextCash::String privateKeyFilePathName = Info::instance().path();
            privateKeyFilePathName.pathAppend(".private_key");
            NextCash::FileInputStream privateKeyFile(privateKeyFilePathName);

            if(!privateKeyFile.isValid())
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Failed to open private key file : %s", privateKeyFilePathName.text());
                requestStop();
                return;
            }

            keyText = privateKeyFile.readString(64);
            if(keyData.writeHex(keyText) != 32)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                  "Failed to read private key from file : %s", privateKeyFilePathName.text());
                requestStop();
                return;
            }

            Key privateKey;
            Signature returnSignature;
            privateKey.readPrivate(&keyData);
            if(!privateKey.sign(validHash, returnSignature))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, mName,
                  "Failed to sign return value");
                requestStop();
                return;
            }

            mReceiveBuffer.flush();

            NextCash::Buffer sendData;
            sendData.writeString("acpt:");
            returnSignature.write(&sendData, false);
            keyBuffer.clear();
            returnSignature.write(&keyBuffer, false);
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, mName,
              "Sending accept signature : %s", keyBuffer.readHexString(keyBuffer.remaining()).text());

            mConnectionMutex.lock();
            mConnection->send(&sendData);
            mConnectionMutex.unlock();
            return;
        }

        // Parse message from receive buffer
        NextCash::String command;
        while(mReceiveBuffer.remaining())
            if(mReceiveBuffer.readByte() == ':' && mReceiveBuffer.readOffset() >= 5)
            {
                mReceiveBuffer.setReadOffset(mReceiveBuffer.readOffset() - 5);
                command = mReceiveBuffer.readString(4);
                mReceiveBuffer.readByte(); // Read colon again
                break;
            }

        if(command.length() != 4)
            return;

        NextCash::Buffer sendData;

        if(command == "clse")
        {
            NextCash::Log::add(NextCash::Log::INFO, mName, "Connection closed");
            requestStop();
            return;
        }
        else if(command == "stat")
        {
            mReceiveBuffer.flush();

            // Return block chain status message
            sendData.writeString("stat:");
            sendData.writeInt(mChain->height());
            if(mChain->isInSync())
                sendData.writeByte(-1);
            else
                sendData.writeByte(0);
            sendData.writeUnsignedInt(mChain->memPool().count());
            sendData.writeUnsignedInt(mChain->memPool().size());

            NextCash::Log::add(NextCash::Log::VERBOSE, mName, "Sending status");
        }
        else if(command == "addr")
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, mName, "Received address request");

            // Return address data message (UTXOs, balances, spent/unspent)
            unsigned int addressLength = mReceiveBuffer.readByte();
            NextCash::String address = mReceiveBuffer.readString(addressLength);
            NextCash::Hash addressHash;
            AddressType addressType;
            AddressFormat addressFormat;

            if(!decodeAddress(address, addressHash, addressType, addressFormat))
            {
                NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                  "Invalid address (%d bytes) : %s", addressLength, address.text());
                sendData.writeString("fail:Invalid Address Format", true);
            }
            else
            {
                if(addressType != PUB_KEY_HASH)
                {
                    NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                      "Wrong address type (%d bytes) : %s", addressLength, address.text());
                    sendData.writeString("fail:Not Public Key Hash", true);
                }
                else
                {
                    std::vector<FullOutputData> outputs;
                    if(!mChain->addresses().getOutputs(addressHash, outputs))
                    {
                        NextCash::Log::addFormatted(NextCash::Log::INFO, mName,
                          "Failed to get outputs for address : %s", address.text());
                        sendData.writeString("fail:No transactions found", true);
                    }
                    else
                    {
                        TransactionOutputPool::Iterator reference;
                        OutputReference *outputReference;

                        sendData.writeString("outp:");
                        sendData.writeUnsignedInt(outputs.size());

                        for(std::vector<FullOutputData>::iterator output=outputs.begin();output!=outputs.end();++output)
                        {
                            output->transactionID.write(&sendData);
                            sendData.writeUnsignedInt(output->index);
                            sendData.writeLong(output->output.amount);

                            reference = mChain->outputs().get(output->transactionID);
                            if(reference)
                            {
                                outputReference = ((TransactionReference *)*reference)->outputAt(output->index);
                                if(outputReference != NULL)
                                    sendData.writeUnsignedInt(outputReference->spentBlockHeight);
                                else
                                    sendData.writeUnsignedInt(-1); // Not spent
                            }
                            else
                                sendData.writeUnsignedInt(-1); // Not spent
                        }
                    }

                    NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
                      "Sending %d outputs for address : %s", outputs.size(), address.text());
                }
            }
        }
        else if(command == "blkd")
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, mName, "Received block details request");

            // Return block details
            int height = mReceiveBuffer.readInt(); // Start height
            int count = mReceiveBuffer.readByte(); // Number of blocks to include
            Block block;
            unsigned int resultCount = 0, inputCount, outputCount;

            if(height < 0)
                height = mChain->height();

            sendData.writeString("blkd:");
            sendData.writeByte(0);

            // Height, Hash, Size, Transaction Count, Input Count, Output Count
            for(int i=0;i<count;++i)
            {
                // Get Block at height - i
                if(!mChain->getBlock((unsigned int)(height - i), block))
                    break;

                sendData.writeUnsignedInt(height - i); // Height
                block.hash.write(&sendData); // Hash
                sendData.writeUnsignedInt(block.time); // Time
                sendData.writeUnsignedInt(block.size()); // Size
                sendData.writeUnsignedLong(block.actualCoinbaseAmount() - coinBaseAmount(height - i)); // Fees
                sendData.writeUnsignedInt(block.transactions.size()); // Transaction Count

                inputCount = 0;
                outputCount = 0;
                for(std::vector<Transaction *>::iterator trans=block.transactions.begin();trans!=block.transactions.end();++trans)
                {
                    inputCount += (*trans)->inputs.size();
                    outputCount += (*trans)->outputs.size();
                }
                sendData.writeUnsignedInt(inputCount); // Input Count
                sendData.writeUnsignedInt(outputCount); // Output Count

                ++resultCount;
            }

            // Update result count
            sendData.setWriteOffset(5);
            sendData.writeByte(resultCount);

            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending %d block details starting at height %d", resultCount, height);
        }
        else if(command == "bkst")
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, mName, "Received block statistics request");

            // Return statistics
            int height = mReceiveBuffer.readInt(); // Start height
            unsigned int hours = mReceiveBuffer.readUnsignedInt(); // Number of hours back in time to include
            uint32_t stopTime = getTime() - (hours * 3600);
            unsigned int blockCount = 0, totalTransactionCount = 0, totalInputCount = 0, totalOutputCount = 0;
            unsigned int inputCount, outputCount;
            uint64_t totalBlockSize = 0, totalFees = 0, fee;
            std::vector<uint64_t> blockSizes, fees;
            std::vector<unsigned int> transactionCounts, inputCounts, outputCounts;
            Block block;

            fees.reserve(256);
            blockSizes.reserve(256);
            transactionCounts.reserve(256);
            inputCounts.reserve(256);
            outputCounts.reserve(256);

            if(hours <= 168) // Week max
            {
                if(height < 0)
                    height = mChain->height();

                int currentHeight = height;

                while(currentHeight > 0)
                {
                    // Get Block at current height
                    if(!mChain->getBlock((unsigned int)currentHeight--, block))
                    {
                        blockCount = 0;
                        totalTransactionCount = 0;
                        totalInputCount = 0;
                        totalOutputCount = 0;
                        totalBlockSize = 0;
                        break;
                    }

                    if(block.time < stopTime)
                        break;

                    // Count inputs and outputs
                    inputCount = 0;
                    outputCount = 0;
                    for(std::vector<Transaction *>::iterator trans=block.transactions.begin();trans!=block.transactions.end();++trans)
                    {
                        inputCount += (*trans)->inputs.size();
                        outputCount += (*trans)->outputs.size();
                    }

                    totalBlockSize += block.size();
                    blockSizes.push_back(block.size());

                    totalTransactionCount += block.transactions.size();
                    transactionCounts.push_back(block.transactions.size());

                    totalInputCount += inputCount;
                    inputCounts.push_back(inputCount);

                    totalOutputCount += outputCount;
                    outputCounts.push_back(outputCount);

                    fee = block.actualCoinbaseAmount() - coinBaseAmount(currentHeight + 1);
                    totalFees += fee;
                    fees.push_back(fee);

                    ++blockCount;
                }
            }

            uint64_t medianBlockSize = 0;
            unsigned int medianTransactionCount = 0;
            unsigned int medianInputCount = 0;
            unsigned int medianOutputCount = 0;
            uint64_t medianFees = 0;

            if(blockCount > 1)
            {
                std::sort(blockSizes.begin(), blockSizes.end());
                medianBlockSize = blockSizes[blockSizes.size()/2];

                std::sort(transactionCounts.begin(), transactionCounts.end());
                medianTransactionCount = transactionCounts[transactionCounts.size()/2];

                std::sort(inputCounts.begin(), inputCounts.end());
                medianInputCount = inputCounts[inputCounts.size()/2];

                std::sort(outputCounts.begin(), outputCounts.end());
                medianOutputCount = outputCounts[outputCounts.size()/2];

                std::sort(fees.begin(), fees.end());
                medianFees = fees[fees.size()/2];
            }
            else if(blockCount == 1)
            {
                medianBlockSize = totalBlockSize;
                medianTransactionCount = totalTransactionCount;
                medianInputCount = totalInputCount;
                medianOutputCount = totalOutputCount;
                medianFees = totalFees;
            }

            sendData.writeString("bkst:");
            sendData.writeUnsignedInt(blockCount); // Total blocks

            sendData.writeUnsignedLong(totalBlockSize); // Total block size
            sendData.writeUnsignedLong(medianBlockSize); // Median block size

            sendData.writeUnsignedInt(totalTransactionCount); // Total transactions
            sendData.writeUnsignedInt(medianTransactionCount); // Median transactions

            sendData.writeUnsignedInt(totalInputCount); // Total Input Count
            sendData.writeUnsignedInt(medianInputCount); // Median Input Count

            sendData.writeUnsignedInt(totalOutputCount); // Total Output Count
            sendData.writeUnsignedInt(medianOutputCount); // Median Output Count

            sendData.writeUnsignedLong(totalFees); // Total Fees
            sendData.writeUnsignedLong(medianFees); // Median Fees

            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Sending block statistics for %d blocks starting at height %d going back %d hours", blockCount, height, hours);
        }
        else if(command == "trxn")
        {
            // Return transaction

        }
        else if(command == "head")
        {
            // Return header

        }
        else if(command == "blok")
        {
            // Return block for specified hash

        }
        else if(command == "blkn")
        {
            // Return block hash at specified height

        }
        else
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, mName,
              "Unknown command : %s", command.text());

        sendData.setReadOffset(0);
        if(sendData.length())
        {
            mLastReceiveTime = getTime();
            mConnectionMutex.lock();
            mConnection->send(&sendData);
            mConnectionMutex.unlock();
        }
    }
}