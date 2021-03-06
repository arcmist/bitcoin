/**************************************************************************
 * Copyright 2018 NextCash, LLC                                           *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "peer.hpp"

#include "base.hpp"


namespace BitCoin
{
    void Peer::write(NextCash::OutputStream *pStream) const
    {
        // Validation Header
        pStream->writeString(START_STRING);

        // User Agent Bytes
        writeCompactInteger(pStream, userAgent.length());

        // User Agent
        pStream->writeString(userAgent);

        // Rating
        pStream->writeInt(rating);

        // Time
        pStream->writeUnsignedInt(time);

        // Services
        pStream->writeUnsignedLong(services);

        // Address
        address.write(pStream);

        // Chain ID
        pStream->writeUnsignedInt(chainID);
    }

    bool Peer::read(NextCash::InputStream *pStream, unsigned int pVersion)
    {
        static const char *match = START_STRING;
        bool matchFound = false;
        unsigned int matchOffset = 0;

        // Search for start string
        while(pStream->remaining())
        {
            if(pStream->readByte() == match[matchOffset])
            {
                matchOffset++;
                if(matchOffset == 4)
                {
                    matchFound = true;
                    break;
                }
            }
            else
                matchOffset = 0;
        }

        if(!matchFound)
            return false;

        // User Agent Bytes
        uint64_t userAgentLength = readCompactInteger(pStream);

        if(userAgentLength > 256)
            return false;

        // User Agent
        userAgent = pStream->readString(userAgentLength);

        // Rating
        rating = pStream->readInt();

        // Time
        time = pStream->readUnsignedInt();

        // Services
        services = pStream->readUnsignedLong();

        // Address
        if(!address.read(pStream))
            return false;

        if(pVersion > 1)
        {
            try
            {
                chainID = static_cast<ChainID>(pStream->readUnsignedInt());
            }
            catch(...)
            {
                return false;
            }
        }
        else
            chainID = CHAIN_UNKNOWN;
        return true;
    }
}
