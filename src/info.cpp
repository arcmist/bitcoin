/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "info.hpp"

#include "buffer.hpp"
#include "file_stream.hpp"
#include "network.hpp"
#include "log.hpp"
#include "digest.hpp"
#include "email.hpp"
#include "message.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <algorithm>

#define BITCOIN_INFO_LOG_NAME "Info"


namespace BitCoin
{
    void notify(const char *pSubject, const char *pMessage)
    {
        NextCash::String emailAddress = Info::instance().notifyEmail;
        if(!emailAddress)
            return;

        NextCash::Email::send(NULL, emailAddress, pSubject, pMessage);
    }

    Info *Info::sInstance = NULL;
    NextCash::Mutex Info::sMutex("Info");
    NextCash::String Info::sPath;

    void Info::setPath(const char *pPath)
    {
        sMutex.lock();
        sPath = pPath;
        sMutex.unlock();
    }

    Info &Info::instance()
    {
        sMutex.lock();
        if(sInstance == NULL)
        {
            sInstance = new Info;
            std::atexit(destroy);
        }
        sMutex.unlock();

        return *sInstance;
    }

    void Info::destroy()
    {
        sMutex.lock();
        if(sInstance != NULL)
            delete sInstance;
        sInstance = NULL;
        sMutex.unlock();
    }

    Info::Info() : mPeerLock("Peer")
    {
        std::memset(ip, 0, INET6_ADDRLEN);

        // Unknown IP Address
        ip[10] = 255;
        ip[11] = 255;
        ip[12] = 127;
        ip[13] = 0;
        ip[14] = 0;
        ip[15] = 1;

        port = 8333;
#ifdef ANDROID
        spvMode = true;
#else
        spvMode = false;
#endif
        maxConnections = 64;
        minFee = 1000; // satoshis per KiB
        mPeersModified = false;
        pendingSizeThreshold = 104857600; // 100 MiB
        pendingBlocksThreshold = 256;
        outputsThreshold = 1073741824; // 1 GiB
        memPoolThreshold = 536870912; // 512 MiB
        addressesThreshold = 268435456; // 256 MiB
        merkleBlockCountRequired = 4;
        spvMemPoolCountRequired = 4;

        mDataModified = false;
        mInitialBlockDownloadComplete = false;

        if(sPath)
        {
            NextCash::String configPath = sPath;
            configPath.pathAppend("config");
            NextCash::FileInputStream configFile(configPath);
            if(configFile.isValid())
                readSettingsFile(&configFile);
        }

        // Initialize random number generator
        std::srand((unsigned int)std::time(0));
    }

    Info::~Info()
    {
        writeDataFile();
        writePeersFile();

        mPeerLock.writeLock("Destroy");
        for(std::list<Peer *>::iterator i = mPeers.begin(); i != mPeers.end(); ++i)
            delete *i;
        mPeerLock.writeUnlock();
    }

    bool Info::load()
    {
        if(!readDataFile())
            return false;
        if(!readPeersFile())
            return false;
        return true;
    }

    void Info::save()
    {
        writeDataFile();
        writePeersFile();
    }

    void Info::applyValue(NextCash::Buffer &pName, NextCash::Buffer &pValue)
    {
        char *name = new char[pName.length()+1];
        pName.read(name, pName.length());
        name[pName.length()] = '\0';

        if(name[0] == '#')
        {
            // Commented line
            delete[] name;
            return;
        }

        char *value = new char[pValue.length()+1];
        pValue.read(value, pValue.length());
        value[pValue.length()] = '\0';

        if(std::strcmp(name, "spv_mode") == 0)
            spvMode = true;
        else if(std::strcmp(name, "max_connections") == 0)
        {
            maxConnections = std::strtol(value, NULL, 0);
            if(maxConnections > 5000)
                maxConnections = 1;
            else if(maxConnections > 128)
                maxConnections = 128;
        }
        else if(std::strcmp(name, "fee_min") == 0)
        {
            minFee = std::strtol(value, NULL, 0);
            if(minFee < 1)
                minFee = 1;
            else if(minFee > 100000)
                minFee = 100000;
        }
        else if(std::strcmp(name, "ip") == 0)
        {
            uint8_t *newIP = NextCash::Network::parseIP(value);
            if(newIP != NULL)
            {
                std::memcpy(ip, newIP, INET6_ADDRLEN);
                delete[] newIP;
            }
        }
        else if(std::strcmp(name, "port") == 0)
            port = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "pending_size") == 0)
            pendingSizeThreshold = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "pending_blocks") == 0)
            pendingBlocksThreshold = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "output_threshold") == 0)
            outputsThreshold = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "mem_pool_size") == 0)
            memPoolThreshold = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "address_threshold") == 0)
            addressesThreshold = std::strtol(value, NULL, 0);
        else if(std::strcmp(name, "notify_email") == 0)
            notifyEmail = value;

        delete[] name;
        delete[] value;
    }

    void Info::readSettingsFile(NextCash::InputStream *pStream)
    {
        char newByte;
        bool equalFound = false;
        NextCash::Buffer name, value;

        while(pStream->remaining())
        {
            newByte = pStream->readByte();

            if(!equalFound && newByte == '=')
                equalFound = true;
            else if(newByte == '\n')
            {
                applyValue(name, value);
                equalFound = false;
                name.clear();
                value.clear();
            }
            else if(!equalFound)
                name.writeByte(newByte);
            else
                value.writeByte(newByte);
        }

        applyValue(name, value);
    }

    bool Info::readDataFile()
    {
        if(!sPath)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_INFO_LOG_NAME,
              "No Path. Not reading data file.");
            return false;
        }

        NextCash::String dataFilePath = sPath;
        dataFilePath.pathAppend("data");

        if(!NextCash::fileExists(dataFilePath))
            return true;

        NextCash::FileInputStream file(dataFilePath);

        uint32_t version = file.readUnsignedInt();

        if(version != 1)
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_INFO_LOG_NAME,
              "Data file version %d not supported", version);
            return false;
        }

        mInitialBlockDownloadComplete = file.readByte() != 0;

        return true;
    }

    void Info::writeDataFile()
    {
        if(!mDataModified)
            return;

        if(!sPath)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_INFO_LOG_NAME,
              "No Path. Not writing data file.");
            return;
        }

        // Write to temp file
        NextCash::String dataFileTempPath = sPath;
        dataFileTempPath.pathAppend("data.temp");
        NextCash::FileOutputStream file(dataFileTempPath, true);

        file.writeUnsignedInt(1); // Version

        // IBD Complete
        if(mInitialBlockDownloadComplete)
            file.writeByte(0x01);
        else
            file.writeByte(0x00);

        file.close();

        // Rename to actual file
        NextCash::String dataFilePath = sPath;
        dataFilePath.pathAppend("data");
        NextCash::renameFile(dataFileTempPath, dataFilePath);

        mDataModified = false;

        //NextCash::String dataFilePath = sPath;
        //dataFilePath.pathAppend("data");
        //NextCash::FileOutputStream file(dataFilePath, true);
    }

    void Info::writePeersFile()
    {
        if(!mPeersModified)
            return;

        if(!sPath)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_INFO_LOG_NAME,
              "No Path. Not writing peers file.");
            return;
        }

        // Write to temp file
        NextCash::String dataFileTempPath = sPath;
        dataFileTempPath.pathAppend("peers.temp");
        NextCash::FileOutputStream file(dataFileTempPath, true);
        file.setOutputEndian(NextCash::Endian::LITTLE);

        mPeerLock.readLock();
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_INFO_LOG_NAME,
          "Writing peers file with %d peers", mPeers.size());
        for(std::list<Peer *>::iterator i = mPeers.begin(); i != mPeers.end(); ++i)
            (*i)->write(&file);
        mPeerLock.readUnlock();

        file.close();

        // Rename to actual file
        NextCash::String dataFilePath = sPath;
        dataFilePath.pathAppend("peers");
        NextCash::renameFile(dataFileTempPath, dataFilePath);

        mPeersModified = false;
    }

    bool Info::readPeersFile()
    {
        if(!sPath)
            return false;

        NextCash::String dataFilePath = sPath;
        dataFilePath.pathAppend("peers");
        NextCash::FileInputStream file(dataFilePath);
        file.setInputEndian(NextCash::Endian::LITTLE);

        if(!file.isValid())
            return true;

        mPeerLock.writeLock("Load");
        for(std::list<Peer *>::iterator i = mPeers.begin(); i != mPeers.end(); ++i)
            delete (*i);
        mPeers.clear();

        Peer *newPeer;
        while(file.remaining())
        {
            newPeer = new Peer();
            if(newPeer->read(&file))
                mPeers.push_back(newPeer);
            else
                break;
        }

        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_INFO_LOG_NAME,
          "Read peers file with %d peers", mPeers.size());
        mPeerLock.writeUnlock();
        return true;
    }

    void Info::getRandomizedPeers(std::vector<Peer *> &pPeers, int pMinimumRating,
      uint64_t mServicesRequiredMask)
    {
        pPeers.clear();

        // For scenario when path was not set before loading instance
        if(mPeers.size() == 0)
            readPeersFile();

        mPeerLock.readLock();
        for(std::list<Peer *>::iterator peer = mPeers.begin(); peer != mPeers.end(); ++peer)
            if((*peer)->rating >= pMinimumRating && ((*peer)->services &
              mServicesRequiredMask) == mServicesRequiredMask)
                pPeers.push_back(*peer);
        mPeerLock.readUnlock();

        // Sort Randomly
        std::random_shuffle(pPeers.begin(), pPeers.end());
    }

    void Info::addPeerFail(const IPAddress &pAddress, int pCount, int pMinimum)
    {
        if(!pAddress.isValid())
            return;

        // For scenario when path was not set before loading instance
        if(mPeers.size() == 0)
            readPeersFile();

        //bool remove = false;
        mPeerLock.readLock();
        for(std::list<Peer *>::iterator peer = mPeers.begin(); peer != mPeers.end(); ++peer)
            if((*peer)->address.matches(pAddress))
            {
                // Update
                if((*peer)->rating > pMinimum)
                {
                    (*peer)->rating -= pCount;
                    if((*peer)->rating < pMinimum)
                        (*peer)->rating = pMinimum;
                }
                (*peer)->updateTime();
                // if((*peer)->rating < 0)
                    // remove = true;
                mPeersModified = true;
                break;
            }
        mPeerLock.readUnlock();

        // if(remove)
        // {
            // mPeerLock.writeLock("Remove");
            // for(std::list<Peer *>::iterator peer=mPeers.begin();peer!=mPeers.end();++peer)
                // if((*peer)->address.matches(pAddress))
                // {
                    // mPeers.erase(peer);
                    // NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_INFO_LOG_NAME, "Removed peer");
                    // break;
                // }
            // mPeerLock.writeUnlock();
        // }
    }

    void Info::updatePeer(const IPAddress &pAddress, const char *pUserAgent, uint64_t pServices)
    {
        if(!pAddress.isValid() || (pUserAgent != NULL && std::strlen(pUserAgent) > 256) ||
          pServices == 0)
            return;

        // For scenario when path was not set before loading instance
        if(mPeers.size() == 0)
            readPeersFile();

        mPeerLock.readLock();
        for(std::list<Peer *>::iterator peer = mPeers.begin(); peer != mPeers.end(); ++peer)
            if((*peer)->address.matches(pAddress))
            {
                // Update existing
                (*peer)->updateTime();
                (*peer)->services = pServices;
                if(pUserAgent != NULL)
                    (*peer)->userAgent = pUserAgent;
                (*peer)->rating += 5;
                mPeersModified = true;
                mPeerLock.readUnlock();
                return;
            }
        mPeerLock.readUnlock();
    }

    void Info::addPeerSuccess(const IPAddress &pAddress, int pCount)
    {
        if(!pAddress.isValid())
            return;

        // For scenario when path was not set before loading instance
        if(mPeers.size() == 0)
            readPeersFile();

        mPeerLock.readLock();
        for(std::list<Peer *>::iterator peer = mPeers.begin(); peer != mPeers.end(); ++peer)
            if((*peer)->address.matches(pAddress))
            {
                // Update existing
                (*peer)->updateTime();
                (*peer)->rating += 5;
                mPeersModified = true;
                mPeerLock.readUnlock();
                return;
            }
        mPeerLock.readUnlock();
    }

    bool Info::addPeer(const IPAddress &pAddress, uint64_t pServices)
    {
        if(!pAddress.isValid() || (pServices & Message::VersionData::FULL_NODE_BIT) == 0)
            return false;

        // For scenario when path was not set before loading instance
        if(mPeers.size() == 0)
            readPeersFile();

        mPeerLock.readLock();
        for(std::list<Peer *>::iterator peer = mPeers.begin(); peer != mPeers.end(); ++peer)
            if((*peer)->address.matches(pAddress))
            {
                mPeerLock.readUnlock();
                return false;
            }
        mPeerLock.readUnlock();

        // Add new
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_INFO_LOG_NAME,
          "Adding new peer %s", pAddress.text().text());
        Peer *newPeer = new Peer;
        newPeer->rating = 0;
        newPeer->updateTime();
        newPeer->address = pAddress;
        newPeer->services = pServices;

        mPeerLock.writeLock("Add");
        mPeers.push_front(newPeer);
        mPeerLock.writeUnlock();
        mPeersModified = true;
        return true;
    }

    bool Info::test()
    {
        bool success = true;

        return success;
    }
}
