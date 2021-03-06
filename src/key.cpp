/**************************************************************************
 * Copyright 2017-2019 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#include "key.hpp"

#ifdef PROFILER_ON
#include "profiler.hpp"
#include "profiler_setup.hpp"
#endif

#include "log.hpp"
#include "math.hpp"
#include "digest.hpp"
#include "encrypt.hpp"
#include "interpreter.hpp"

#define BITCOIN_KEY_LOG_NAME "Key"


namespace BitCoin
{
    secp256k1_context *Key::sContext = NULL;
    unsigned int Key::sContextFlags = 0;
    NextCash::MutexWithConstantName Key::sMutex("SECP256K1");
    const unsigned int Key::DEFAULT_GAP = 20;
    const uint32_t Key::HARDENED = 0x80000000;
    const uint32_t Key::PURPOSE_44 = HARDENED + 44;
    const uint32_t Key::COIN_BITCOIN = HARDENED;
    const uint32_t Key::COIN_BITCOIN_CASH = HARDENED + 145;
    const uint32_t Key::COIN_BITCOIN_SV = HARDENED + 236;

    void randomizeContext(secp256k1_context *pContext)
    {
        bool finished = false;
        uint8_t entropy[32];
        uint32_t random;
        while(!finished)
        {
            // Generate entropy
            for(unsigned int i=0;i<32;i+=4)
            {
                random = NextCash::Math::randomInt();
                std::memcpy(entropy + i, &random, 4);
            }
            finished = secp256k1_context_randomize(pContext, entropy);
        }
    }

    secp256k1_context *Key::context(unsigned int pFlags)
    {
        sMutex.lock();
        if(sContext == NULL)
        {
            // Create context
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Creating initial context : %08x", pFlags);
            sContext = secp256k1_context_create(pFlags);
            // sContext = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
            sContextFlags = pFlags;
            if(pFlags & SECP256K1_FLAGS_BIT_CONTEXT_SIGN)
                randomizeContext(sContext);
            std::atexit(destroyContext);
        }
        else if((sContextFlags & pFlags) != pFlags)
        {
            // Recreate context with new flags
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Recreating context : %08x", pFlags);
            secp256k1_context_destroy(sContext);
            sContextFlags |= pFlags;
            sContext = secp256k1_context_create(sContextFlags);
            if(sContextFlags & SECP256K1_FLAGS_BIT_CONTEXT_SIGN)
                randomizeContext(sContext);
        }
        sMutex.unlock();

        return sContext;
    }

    void Key::destroyContext()
    {
        secp256k1_context_destroy(sContext);
        sContext = NULL;
    }

    NextCash::String Signature::hex() const
    {
        NextCash::String result;
        result.writeHex(mData, 64);
        return result;
    }

    uint64_t cashAddressCheckSum(NextCash::InputStream *pData)
    {
        uint64_t result = 1;
        uint8_t round;
        while(pData->remaining())
        {
            round = result >> 35;
            result = ((result & 0x07ffffffff) << 5) ^ pData->readByte();

            if(round & 0x01) result ^= 0x98f2bc8e61;
            if(round & 0x02) result ^= 0x79b76d99e2;
            if(round & 0x04) result ^= 0xf33e5fb3c4;
            if(round & 0x08) result ^= 0xae2eabe2a8;
            if(round & 0x10) result ^= 0x1e4f43e470;
        }

        return result ^ 0x01;
    }

    NextCash::String encodePaymentCode(const NextCash::Hash &pHash,
      PaymentRequest::Format pFormat, AddressType pType, uint64_t pAmount, NextCash::String pLabel,
      NextCash::String pMessage)
    {
        NextCash::String result;

        switch(pFormat)
        {
        case PaymentRequest::Format::LEGACY:
            result = "bitcoin:";
            result += encodeLegacyAddress(pHash, pType);
            break;
        case PaymentRequest::Format::CASH:

            switch(pType)
            {
                default:
                case MAIN_PUB_KEY_HASH: // Mainnet Public key hash
                case MAIN_SCRIPT_HASH: // Mainnet Script hash
                case MAIN_PRIVATE_KEY: // Mainnet Private key
                    result = "bitcoincash:";
                    break;
                case TEST_PUB_KEY_HASH: // Testnet Public key hash
                case TEST_SCRIPT_HASH: // Testnet Script hash
                case TEST_PRIVATE_KEY: // Testnet Private key
                    result = "bchtest:";
                    break;
            }

            result += encodeCashAddress(pHash, pType);
            break;
        default:
        case PaymentRequest::Format::INVALID:
            return NextCash::String();
            break;
        }

        bool isFirstParameter = true;

        if(pAmount != 0)
        {
            NextCash::String amountString;
            amountString.writeFormatted("amount=%.2f", bitcoins(pAmount));
            if(isFirstParameter)
                result += "?";
            else
                result += "&";
            result += amountString;
        }

        if(pLabel)
        {
            if(isFirstParameter)
                result += "?";
            else
                result += "&";
            result += "label=";
            result += NextCash::uriEncode(pLabel);
        }

        if(pMessage)
        {
            if(isFirstParameter)
                result += "?";
            else
                result += "&";
            result += "message=";
            result += NextCash::uriEncode(pMessage);
        }

        return result;
    }

    NextCash::String encodeLegacyAddress(const NextCash::Hash &pHash, AddressType pType)
    {
        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        NextCash::Buffer data, check;

        // Calculate check
        digest.writeByte(pType);
        pHash.write(&digest);
        digest.getResult(&check);

        // Write data for address
        data.writeByte(pType);
        pHash.write(&data);
        data.writeUnsignedInt(check.readUnsignedInt());

        // Encode with base 58
        NextCash::String result;
        result.writeBase58(data.begin(), (unsigned int)data.length());
        return result;
    }

    bool decodeLegacyAddress(const char *pText, NextCash::Hash &pHash, AddressType &pType)
    {
        NextCash::Buffer data;

        // Parse address into public key hash
        data.writeBase58AsBinary(pText);

        if(data.length () == 0)
            return false;

        try
        {
            pType = static_cast<AddressType>(data.readByte());
        }
        catch(...)
        {
            return false;
        }

        if(pType == MAIN_PRIVATE_KEY || pType == TEST_PRIVATE_KEY)
            return false;

        if(data.length() < 24 || data.length() > 35)
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_KEY_LOG_NAME,
              "Invalid legacy address length for type %02x : %d not within (24, 35)", pType,
              data.length());
            return false;
        }

        pHash.setSize(data.remaining() - 4);
        pHash.writeStream(&data, data.remaining() - 4);

        uint32_t check = data.readUnsignedInt();

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        data.setReadOffset(0);
        data.readStream(&digest, data.length() - 4);
        NextCash::Buffer checkHash;
        digest.getResult(&checkHash);

        uint32_t checkValue = checkHash.readUnsignedInt();
        if(checkValue != check)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Invalid legacy address check : %08x != %08x", checkValue, check);
            return false;
        }

        return true;
    }

    NextCash::String encodeCashAddress(const NextCash::Hash &pHash, AddressType pType)
    {
        if(pType == MAIN_PRIVATE_KEY || pType == TEST_PRIVATE_KEY)
            return NextCash::String(); // Not supported

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        NextCash::Buffer data, check;

        uint8_t versionByte = 0; // Top bit zero, next 4 type, next 3 size
        const char *prefix;
        switch(pType)
        {
            default:
            case MAIN_PUB_KEY_HASH: // Mainnet Public key hash
            case MAIN_SCRIPT_HASH: // Mainnet Script hash
            case MAIN_PRIVATE_KEY: // Mainnet Private key
                prefix = "bitcoincash";
                break;
            case TEST_PUB_KEY_HASH: // Testnet Public key hash
            case TEST_SCRIPT_HASH: // Testnet Script hash
            case TEST_PRIVATE_KEY: // Testnet Private key
                prefix = "bchtest";
                break;
        }

        if(pType == MAIN_SCRIPT_HASH || pType == TEST_SCRIPT_HASH)
            versionByte |= (0x01 << 3);

        if(pHash.size() == 20) // 160 bits
            versionByte |= 0x00;
        else if(pHash.size() == 24) // 192 bits
            versionByte |= 0x01;
        else if(pHash.size() == 28) // 224 bits
            versionByte |= 0x02;
        else if(pHash.size() == 32) // 256 bits
            versionByte |= 0x03;
        else if(pHash.size() == 40) // 320 bits
            versionByte |= 0x04;
        else if(pHash.size() == 48) // 384 bits
            versionByte |= 0x05;
        else if(pHash.size() == 56) // 448 bits
            versionByte |= 0x06;
        else //if(pHash.size() == 64) // 512 bits
            versionByte |= 0x07;

        data.writeByte(versionByte);
        pHash.write(&data);

        // Encode with base 32
        NextCash::String encodedPayload;
        encodedPayload.writeBase32(data.begin(), (unsigned int)data.length());

        // Build check sum data
        NextCash::Buffer checkSumData;
        std::vector<bool> bits;
        uint8_t byteValue;
        unsigned int bitOffset;

        // Prefix
        const char *prefixChar = prefix;
        while(*prefixChar)
        {
            checkSumData.writeByte((uint8_t)*prefixChar & (uint8_t)0x1f);
            ++prefixChar;
        }

        // Separator
        checkSumData.writeByte(0);

        // Payload
        data.setReadOffset(0);
        while(data.remaining())
        {
            byteValue = data.readByte();
            for(int bitOffsetIter=0;bitOffsetIter<8;++bitOffsetIter)
                bits.push_back(NextCash::Math::bit(byteValue, bitOffsetIter));
        }

        // Pad payload to 5 bit boundary
        while(bits.size() % 5)
            bits.push_back(0);

        // Convert 5 bit sets back to 5 bit bytes
        bitOffset = 0;
        byteValue = 0;
        for(std::vector<bool>::iterator bit=bits.begin();bit!=bits.end();++bit)
        {
            byteValue <<= 1;
            if(*bit)
                byteValue |= 0x01;
            ++bitOffset;

            if(bitOffset == 5)
            {
                checkSumData.writeByte(byteValue);
                byteValue = 0;
                bitOffset = 0;
            }
        }

        // Check sum template (8 x 5 zero bits)
        for(unsigned int i=0;i<8;++i)
            checkSumData.writeByte(0);

        // Calculate check sum
        uint64_t checkSum = cashAddressCheckSum(&checkSumData);

        // Append check sum to result
        NextCash::Buffer encodedCheckSum;
        for(int i=0;i<8;++i)
            encodedCheckSum.writeByte((uint8_t)NextCash::Math::base32Codes[(checkSum >> (5 * (7 - i))) & 0x1f]);

        return encodedPayload + encodedCheckSum.readString(encodedCheckSum.length());
    }

    bool decodeCashAddress(const char *pText, NextCash::Hash &pHash, AddressType &pType)
    {
        const char *character = pText;
        NextCash::Buffer prefixBuffer, checkSumData;
        while(*character && *character != ':')
        {
            prefixBuffer.writeByte((uint8_t)NextCash::lower(*character));
            ++character;
        }

        if(*character == ':')
        {
            ++character;
            if(prefixBuffer.length() == 0)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Cash address with zero length prefix");
                return false;
            }
        }
        else
        {
            prefixBuffer.clear();
            character = pText;
        }

        NextCash::String prefix = prefixBuffer.readString(prefixBuffer.length());
        unsigned int remainingLength = std::strlen(character);

        if(remainingLength < 8)
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Cash address payload less than 8 characters");
            return false;
        }

        NextCash::Buffer payload, decodedPayload, checkSumPayload;
        const char *match;

        payload.write(character, remainingLength - 8);
        payload.writeByte(0); // Write null byte for base 32 convert
        decodedPayload.writeBase32AsBinary((const char *)payload.begin());

        while(payload.remaining() > 1) // Don't include null byte
        {
            // Decode base32 character
            match = std::strchr(NextCash::Math::base32Codes, NextCash::lower(payload.readByte()));
            if(match == NULL)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Cash address with invalid base32 character");
                return false;
            }
            checkSumPayload.writeByte(match - NextCash::Math::base32Codes);
        }

        // Write check sum
        character += (remainingLength - 8);
        while(*character)
        {
            // Decode base32 character
            match = std::strchr(NextCash::Math::base32Codes, NextCash::lower(*character));
            if(match == NULL)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Cash address check sum with invalid base32 character");
                return false;
            }
            checkSumPayload.writeByte(match - NextCash::Math::base32Codes);
            ++character;
        }

        // Verify checksum
        std::vector<NextCash::String> prefixesToAttempt;
        bool validCheckSum = false;

        if(prefix.length())
            prefixesToAttempt.push_back(prefix);
        else
        {
            prefixesToAttempt.push_back(NextCash::String("bitcoincash"));
            prefixesToAttempt.push_back(NextCash::String("bchtest"));
        }

        for(std::vector<NextCash::String>::iterator prefixAttempt=prefixesToAttempt.begin();prefixAttempt!=prefixesToAttempt.end();++prefixAttempt)
        {
            checkSumData.clear();
            character = prefixAttempt->text();

            // Prefix
            while(*character)
            {
                checkSumData.writeByte(*character & 0x1f);
                ++character;
            }

            // Separator
            checkSumData.writeByte(0);

            // Payload
            checkSumPayload.setReadOffset(0);
            checkSumData.writeStream(&checkSumPayload, checkSumPayload.length());

            if(cashAddressCheckSum(&checkSumData) == 0)
            {
                validCheckSum = true;
                if(*prefixAttempt == "bitcoincash")
                    pType = MAIN_PUB_KEY_HASH;
                else if(*prefixAttempt == "bchtest")
                    pType = TEST_PUB_KEY_HASH;
                else
                    pType = UNKNOWN;
                break;
            }
        }

        if(!validCheckSum)
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Cash address valid check sum not found for given prefixes");
            pType = UNKNOWN;
            return false;
        }

        uint8_t versionByte = decodedPayload.readByte();
        unsigned int decodedSize = 0;
        switch(versionByte & 0x07) // 3 least significant bits
        {
        case 0: // 160
            decodedSize = 20;
            break;
        case 1: // 192
            decodedSize = 24;
            break;
        case 2: // 224
            decodedSize = 28;
            break;
        case 3: // 256
            decodedSize = 32;
            break;
        case 4: // 320
            decodedSize = 40;
            break;
        case 5: // 384
            decodedSize = 48;
            break;
        case 6: // 448
            decodedSize = 56;
            break;
        case 7: // 512
            decodedSize = 64;
            break;
        default:
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Cash address encoded size is not valid : %d", versionByte & 0x07);
            return false;
        }

        switch((versionByte >> 3) & 0x0f)
        {
        case 0: // P2KH
            // Already set as main net pub key hash
            break;
        case 1: // P2SH
            if(pType == MAIN_PUB_KEY_HASH)
                pType = MAIN_SCRIPT_HASH;
            else if(pType == TEST_PUB_KEY_HASH)
                pType = TEST_SCRIPT_HASH;
            break;
        default:
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Cash address encoded type is not valid : %d", (versionByte >> 3) & 0x0f);
            break;
        }

        return pHash.read(&decodedPayload, decodedSize);
    }

    PaymentRequest decodePaymentCode(const char *pText)
    {
        PaymentRequest result;
        NextCash::String prefix, address, label, message, amount;
        bool parameterStart = false;
        bool valueStart = false;
        NextCash::String name, value;

        for(const char *ptr = pText; *ptr != '\0'; ++ptr)
        {
            if(parameterStart)
            {
                if(valueStart)
                {
                    if(*ptr == '&')
                    {
                        // Process value
                        if(name == "label")
                            result.label = NextCash::uriDecode(value);
                        else if(name == "message")
                            result.message = NextCash::uriDecode(value);
                        else if(name == "amount")
                        {
                            // Parse amount as double of bitcoins
                            double bitcoins = std::atof(value.text());
                            // Convert to satoshis for result.amount
                            result.amount = (uint64_t)satoshisFromBitcoins(bitcoins);
                            result.amountSpecified = true;
                        }
                        else if(name == "r")
                        {
                            result.secureURL = NextCash::uriDecode(value);
                            result.type = BIP0070;
                        }
                        else if(name.length() >= 4 && std::memcmp(name.text(), "req-", 4) == 0)
                        {
                            // Unknown required value
                            result.format = PaymentRequest::Format::INVALID;
                            result.type = AddressType::UNKNOWN;
                            return result;
                        }

                        valueStart = false;
                        value = "";
                        name = "";
                    }
                    else
                        value += *ptr;
                }
                else if(*ptr == '=')
                    valueStart = true;
                else if(*ptr == '&')
                    name.clear();  // No value found
                else
                    name += *ptr;
            }
            else if(*ptr == '?')
                parameterStart = true;
            else if(*ptr == ':')
            {
                prefix = address;
                address.clear();
            }
            else
                address += *ptr;
        }

        // Process value
        if(name == "label")
            result.label = NextCash::uriDecode(value);
        else if(name == "message")
            result.message = NextCash::uriDecode(value);
        else if(name == "amount")
        {
            // Parse amount as double of bitcoins
            double bitcoins = std::atof(value.text());
            // Convert to satoshis for result.amount
            result.amount = (uint64_t)satoshisFromBitcoins(bitcoins);
            result.amountSpecified = true;
        }
        else if(name == "r")
        {
            result.secureURL = NextCash::uriDecode(value);
            result.type = BIP0070;
        }
        else if(name.length() >= 4 && std::memcmp(name.text(), "req-", 4) == 0)
        {
            // Unknown required value
            result.format = PaymentRequest::Format::INVALID;
            result.type = AddressType::UNKNOWN;
            return result;
        }

        if(prefix == "bitcoin")
            result.format = PaymentRequest::Format::LEGACY;
        else if(prefix == "bitcoincash")
            result.format = PaymentRequest::Format::CASH;
        else
            result.format = PaymentRequest::Format::INVALID;

        if(address)
        {
            if(decodeLegacyAddress(address, result.pubKeyHash, result.type))
                result.format = PaymentRequest::Format::LEGACY;
            else if(decodeCashAddress(address, result.pubKeyHash, result.type))
                result.format = PaymentRequest::Format::CASH;
            else if(!result.secureURL)
            {
                result.format = PaymentRequest::Format::INVALID;
                result.type = AddressType::UNKNOWN;
                return result;
            }
        }
        else if(!result.secureURL)
        {
            result.format = PaymentRequest::Format::INVALID;
            result.type = AddressType::UNKNOWN;
            return result;
        }

        switch(result.type)
        {
        case MAIN_SCRIPT_HASH:
            result.network = MAINNET;
            break;
        case AddressType::MAIN_PUB_KEY_HASH:
            result.network = MAINNET;
            break;
        case MAIN_PRIVATE_KEY:
            result.network = MAINNET;
            break;
        case TEST_SCRIPT_HASH:
            result.network = TESTNET;
            break;
        case TEST_PUB_KEY_HASH:
            result.network = TESTNET;
            break;
        case TEST_PRIVATE_KEY:
            result.network = TESTNET;
            break;
        case BIP0070:
            break;
        default:
        case UNKNOWN:
            result.format = PaymentRequest::Format::INVALID;
            break;
        }

        return result;
    }

    void Signature::write(NextCash::OutputStream *pStream, bool pScriptFormat) const
    {
        size_t length = 73;
        uint8_t output[length];
        if(!secp256k1_ecdsa_signature_serialize_der(Key::context(SECP256K1_CONTEXT_NONE), output,
          &length, (secp256k1_ecdsa_signature*)mData))
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Failed to write signature");
        if(pScriptFormat)
            ScriptInterpreter::writePushDataSize(length + 1, pStream);
        pStream->write(output, length);
        pStream->writeByte(mHashType);
    }

    // Hack badly formatted signatures
    // If non-strict signatures are allowed then this function must be called because parse_der
    //   will return true on some signatures that are not formatted correctly and verify will fail.
    // Returns true if a change was made and it might now be a valid signature.
    bool repairSignature(uint8_t *pData, unsigned int &pSize)
    {
        bool result = false;
        uint8_t *ptr = pData;

        if(*ptr++ != 0x30) // Compound header byte
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Invalid compound header byte in signature (%d bytes) : %s", pSize,
              // hex.text());
            return false;
        }

        // Full length
        uint8_t *fullLength = ptr++;
        if(*fullLength != pSize - 2)
        {
            if(*fullLength < pSize - 2)
            {
                // NextCash::String hex;
                // hex.writeHex(pData, pSize);
                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  // "Adjusting parse length %d to match total length in signature %d + 2 (header byte and length byte) : %s",
                  // pSize, *fullLength, hex.text());
                pSize = *fullLength + 2;
                result = true;
            }
            else
            {
                // NextCash::String hex;
                // hex.writeHex(pData, pSize);
                // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  // "Invalid total length byte in signature (%d bytes) : %s", pSize, hex.text());
                return false;
            }
        }

        // Integer header byte
        if(*ptr++ != 0x02)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Invalid R integer header byte in signature (%d bytes) : %s", pSize, hex.text());
            return false;
        }

        // R length
        uint8_t *rLength = ptr++;
        if(*rLength + (ptr - pData) > pSize)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "R integer length byte too high in signature (%d bytes) : %s", pSize, hex.text());
            return false;
        }

        while(*ptr == 0x00 && !(*(ptr+1) & 0x80))
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Removing extra leading zero byte in R value from signature (%d bytes) : %s", pSize,
              // hex.text());

            // Extra padding. Remove this
            std::memmove(ptr, ptr + 1, pSize - (ptr - pData) - 1);

            // Adjust lengths
            --pSize;
            --*rLength;
            --*fullLength;
            result = true;
        }

        if(*ptr & 0x80)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Adding required leading zero byte in R value to signature (%d bytes) : %s", pSize,
              // hex.text());

            // Add a zero byte
            std::memmove(ptr + 1, ptr, pSize - (ptr - pData));
            *ptr = 0x00;

            // Adjust lengths
            ++pSize;
            ++*rLength;
            ++*fullLength;
            result = true;
        }

        ptr += *rLength;

        // Integer header byte
        if(*ptr++ != 0x02)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Invalid S integer header byte in signature (%d bytes) : %s", pSize, hex.text());
            return false;
        }

        // S length
        uint8_t *sLength = ptr++;
        if(*sLength + (ptr - pData) > pSize)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "S integer length byte too high in signature (%d bytes) : %s", pSize, hex.text());
            return false;
        }

        while(*ptr == 0x00 && !(*(ptr+1) & 0x80))
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Removing extra leading zero byte in S value to signature (%d bytes) : %s", pSize,
              // hex.text());

            // Extra padding. Remove this
            std::memmove(ptr, ptr + 1, pSize - (ptr - pData) - 1);

            // Adjust lengths
            --pSize;
            --*sLength;
            --*fullLength;
            result = true;
        }

        if(*ptr & 0x80)
        {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Adding required leading zero byte in S value from signature (%d bytes) : %s", pSize,
              // hex.text());

            // Add a zero byte
            std::memmove(ptr + 1, ptr, pSize - (ptr - pData));
            *ptr = 0x00;

            // Adjust lengths
            ++pSize;
            ++*sLength;
            ++*fullLength;
            result = true;
        }

        // if(result)
        // {
            // NextCash::String hex;
            // hex.writeHex(pData, pSize);
            // NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              // "Repaired signature (%d bytes) : %s", pSize, hex.text());
        // }
        return result;
    }

    bool Signature::read(NextCash::InputStream *pStream, unsigned int pLength,
      bool pStrictECDSA_DER_Sigs)
    {
        if(pLength < 2)
        {
            clear();
            return false;
        }

        uint8_t input[pLength + 2]; // Max of 2 bytes added by repair.
        unsigned int length = pLength - 1;

        std::memset(mData, 0, 64);

        pStream->read(input, length);
        try
        {
            mHashType = static_cast<Signature::HashType>(pStream->readByte());
        }
        catch(...)
        {
            NextCash::String hex;
            hex.writeHex(input, length);
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
              "Invalid signature hash type : %s", length, hex.text());
            return false;
        }

        secp256k1_context *thisContext = Key::context(SECP256K1_CONTEXT_NONE);

        if(!pStrictECDSA_DER_Sigs)
            repairSignature(input, length);

        if(secp256k1_ecdsa_signature_parse_der(thisContext, (secp256k1_ecdsa_signature*)mData,
          input, length))
            return true;

        if(length == 64 && !pStrictECDSA_DER_Sigs)
        {
            if(secp256k1_ecdsa_signature_parse_compact(thisContext,
              (secp256k1_ecdsa_signature*)mData, input))
                return true;
            else
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to parse compact signature (64 bytes)");
                clear();
                return false;
            }
        }

        NextCash::String hex;
        hex.writeHex(input, length);
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
          "Failed to parse signature (%d bytes) : %s", length, hex.text());
        clear();
        return false;
    }

    Key::Key(Key &pCopy) : mChildLock("KeyChild")
    {
        mVersion = pCopy.mVersion;
        mDepth = pCopy.mDepth;
        std::memcpy(mParentFingerPrint, pCopy.mParentFingerPrint, 4);
        mIndex = pCopy.mIndex;
        std::memcpy(mChainCode, pCopy.mChainCode, 32);
        std::memcpy(mKey, pCopy.mKey, 33);
        std::memcpy(mFingerPrint, pCopy.mFingerPrint, 4);

        if(pCopy.mPublicKey != NULL)
            mPublicKey = new Key(*pCopy.mPublicKey);
        else
            mPublicKey = NULL;

        mChildLock.lock();
        pCopy.mChildLock.lock();
        mChildren.reserve(pCopy.mChildren.size());
        for(std::vector<Key *>::iterator key=pCopy.mChildren.begin();key!=pCopy.mChildren.end();++key)
            mChildren.push_back(new Key(**key));
        pCopy.mChildLock.unlock();
        mChildLock.unlock();

        mHash = pCopy.mHash;
        mUsed = pCopy.mUsed;
    }

    void Key::operator = (Key &pRight)
    {
        mVersion = pRight.mVersion;
        mDepth = pRight.mDepth;
        std::memcpy(mParentFingerPrint, pRight.mParentFingerPrint, 4);
        mIndex = pRight.mIndex;
        std::memcpy(mChainCode, pRight.mChainCode, 32);
        std::memcpy(mKey, pRight.mKey, 33);
        std::memcpy(mFingerPrint, pRight.mFingerPrint, 4);

        if(mPublicKey != NULL)
            delete mPublicKey;
        if(pRight.mPublicKey != NULL)
            mPublicKey = new Key(*pRight.mPublicKey);
        else
            mPublicKey = NULL;

        mChildLock.lock();
        for(std::vector<Key *>::iterator key=mChildren.begin();key!=mChildren.end();++key)
            delete *key;
        mChildren.clear();
        pRight.mChildLock.lock();
        mChildren.reserve(pRight.mChildren.size());
        for(std::vector<Key *>::iterator key=pRight.mChildren.begin();key!=pRight.mChildren.end();++key)
            mChildren.push_back(new Key(**key));
        pRight.mChildLock.unlock();
        mChildLock.unlock();

        mHash = pRight.mHash;
        mUsed = pRight.mUsed;
    }

    const NextCash::Hash &Key::hash() const
    {
        if(isPrivate() && mPublicKey != NULL)
            return mPublicKey->hash();
        else
            return mHash;
    }

    NextCash::String Key::address(PaymentRequest::Format pFormat) const
    {
        if(isPrivate())
        {
            if(mPublicKey != NULL)
                return mPublicKey->address();
            else
                return NextCash::String();
        }

        AddressType type;
        switch(mVersion)
        {
            case MAINNET_PUBLIC:
            case MAINNET_PUBKEY_HASH:
                type = AddressType::MAIN_PUB_KEY_HASH;
                break;
            case TESTNET_PUBLIC:
                type = AddressType::TEST_PUB_KEY_HASH;
                break;
            default:
                return NextCash::String();
        }

        switch(pFormat)
        {
        case PaymentRequest::Format::LEGACY:
            return encodeLegacyAddress(hash(), type);
        case PaymentRequest::Format::CASH:
            return encodeCashAddress(hash(), type);
        default:
            return NextCash::String();
        }
    }

    void Key::clear()
    {
        mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
            delete *child;
        mChildren.clear();
        mChildLock.unlock();

        if(mPublicKey != NULL)
            delete mPublicKey;
        mPublicKey = NULL;

        mVersion = EMPTY;
        mDepth = 0;
        std::memset(mParentFingerPrint, 0, 4);
        mIndex = 0;

        // Double clear memory
        std::memset(mChainCode, 0, 32);
        std::memset(mKey, 0, 33);
        std::memset(mChainCode, 0xff, 32);
        std::memset(mKey, 0xff, 33);
        std::memset(mChainCode, 0, 32);
        std::memset(mKey, 0, 33);

        mHash.clear();
        mUsed = false;
    }

    bool Key::readPublic(NextCash::InputStream *pStream)
    {
        clear();

        mDepth = NO_DEPTH;
        mIndex = NO_DEPTH;

        if(pStream->remaining() < 33)
            return false;

        mKey[0] = pStream->readByte();

        if(mKey[0] == 0x04) // Uncompressed
        {
            if(pStream->remaining() < 64)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to read public key. type %02x size %d", mKey[0], pStream->remaining() + 1);
                return false;
            }

            uint8_t data[65];
            data[0] = mKey[0];
            pStream->read(data + 1, 64);

            // Convert to compressed public key
            secp256k1_context *thisContext = context(SECP256K1_CONTEXT_NONE);
            secp256k1_pubkey pubkey;

            if(!secp256k1_ec_pubkey_parse(thisContext, &pubkey, data, 65))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to parse public key");
                return false;
            }

            size_t length = 33;
            if(!secp256k1_ec_pubkey_serialize(context(SECP256K1_CONTEXT_VERIFY), mKey, &length,
              &pubkey, SECP256K1_EC_COMPRESSED) || length != 33)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to compress public key");
                return false;
            }

            // Calculate hash
            NextCash::Digest hash(NextCash::Digest::SHA256_RIPEMD160);
            hash.write(mKey, 33);
            hash.getResult(&mHash);

            return true;
        }
        else if(mKey[0] == 0x02 || mKey[0] == 0x03) // Compressed
        {
            if(pStream->remaining() < 32)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to read public key. type %02x size %d", mKey[0], pStream->remaining() + 1);
                return false;
            }

            pStream->read(mKey + 1, 32);

            // Calculate hash
            NextCash::Digest hash(NextCash::Digest::SHA256_RIPEMD160);
            hash.write(mKey, 33);
            hash.getResult(&mHash);

            return true;
        }
        else // Unknown type
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Public key type unknown. type %02x", mKey[0]);
            return false;
        }
    }

    bool Key::writePublic(NextCash::OutputStream *pStream, bool pScriptFormat) const
    {
        if(isPrivate()) // Private or key missing
            return false;

        if(pScriptFormat)
            ScriptInterpreter::writePushDataSize(33, pStream);
        pStream->write(mKey, 33);
        return true;
    }

    bool Key::readPrivate(NextCash::InputStream *pStream)
    {
        clear();

        mDepth = NO_DEPTH;
        mIndex = NO_DEPTH;

        if(pStream->remaining() < 32)
            return false;

        mVersion = MAINNET_PRIVATE;
        mKey[0] = 0; // Private
        pStream->read(mKey + 1, 32);
        return finalize();
    }

    bool Key::writePrivate(NextCash::OutputStream *pStream, bool pScriptFormat) const
    {
        if(!isPrivate()) // Not private
            return false;

        pStream->write(mKey + 1, 32);
        return true;
    }

    void Key::generatePrivate(Network pNetwork)
    {
        clear();

        secp256k1_context *thisContext = context(SECP256K1_CONTEXT_NONE);

        switch(pNetwork)
        {
        case MAINNET:
            mVersion = MAINNET_PRIVATE;
            break;
        case TESTNET:
            mVersion = TESTNET_PRIVATE;
            break;
        }
        mDepth = NO_DEPTH;
        mIndex = NO_DEPTH;

        while(true)
        {
            // Generate entropy
            unsigned int random;
            for(unsigned int i=0;i<32;i+=4)
            {
                random = NextCash::Math::randomInt();
                std::memcpy(mKey + 1 + i, &random, 4);
            }

            // Check validity
            if(secp256k1_ec_seckey_verify(thisContext, mKey + 1))
            {
                // Create public key
                finalize();
                return;
            }
        }
    }

    void Key::loadHash(const NextCash::Hash &pHash)
    {
        clear();

        mVersion = MAINNET_PUBKEY_HASH;
        mDepth = NO_DEPTH;
        mIndex = NO_DEPTH;
        mHash = pHash;
        mKey[0] = 0xff; // Not private or public
        std::memcpy(mKey + 1, mHash.data(), PUB_KEY_HASH_SIZE);
    }

    void Key::write(NextCash::OutputStream *pStream) const
    {
        pStream->setOutputEndian(NextCash::Endian::BIG);
        switch(mVersion)
        {
        case MAINNET_PRIVATE:
            pStream->writeUnsignedInt(0x0488ADE4);
            break;
        case MAINNET_PUBLIC:
            pStream->writeUnsignedInt(0x0488B21E);
            break;
        case TESTNET_PRIVATE:
            pStream->writeUnsignedInt(0x04358394);
            break;
        case TESTNET_PUBLIC:
            pStream->writeUnsignedInt(0x043587CF);
            break;
        case MAINNET_PUBKEY_HASH:
            pStream->writeUnsignedInt(0x000000fe);
            break;
        case EMPTY:
            pStream->writeUnsignedInt(0x000000ff);
            break;
        }
        pStream->writeByte(mDepth);
        pStream->write(mParentFingerPrint, 4);
        pStream->writeUnsignedInt(mIndex);
        pStream->write(mChainCode, 32);
        pStream->write(mKey, 33);
    }

    bool Key::read(NextCash::InputStream *pStream)
    {
        clear();

        if(pStream->remaining() < 78)
            return false;

        pStream->setInputEndian(NextCash::Endian::BIG);
        uint32_t versionValue = pStream->readUnsignedInt();
        switch(versionValue)
        {
        case 0x0488ADE4:
            mVersion = MAINNET_PRIVATE;
            break;
        case 0x0488B21E:
            mVersion = MAINNET_PUBLIC;
            break;
        case 0x04358394:
            mVersion = TESTNET_PRIVATE;
            break;
        case 0x043587CF:
            mVersion = TESTNET_PUBLIC;
            break;
        case 0x000000fe:
            mVersion = MAINNET_PUBKEY_HASH;
            break;
        case 0x000000ff:
            mVersion = EMPTY;
            break;
        default:
            return false;
        }
        mDepth = pStream->readByte();
        pStream->read(mParentFingerPrint, 4);
        mIndex = pStream->readUnsignedInt();
        pStream->read(mChainCode, 32);
        pStream->read(mKey, 33);

        if(mVersion == MAINNET_PUBKEY_HASH)
            mHash.write(mKey + 1, PUB_KEY_HASH_SIZE);
        else if(!isPrivate())
        {
            // Calculate hash
            NextCash::Digest hash(NextCash::Digest::SHA256_RIPEMD160);
            hash.write(mKey, 33);
            hash.getResult(&mHash);
        }

        return true;
    }

    void Key::writeTree(NextCash::OutputStream *pStream)
    {
        write(pStream);

        if(isEmpty())
            return;

        pStream->writeByte(mUsed);
        if(isPrivate() && mPublicKey != NULL)
            mPublicKey->writeTree(pStream);

        mChildLock.lock();
        pStream->writeUnsignedInt(mChildren.size());
        for(std::vector<Key *>::const_iterator child=mChildren.begin();child!=mChildren.end();++child)
            (*child)->writeTree(pStream);
        mChildLock.unlock();
    }

    bool Key::readTree(NextCash::InputStream *pStream)
    {
        if(!read(pStream))
            return false;

        if(isEmpty())
            return true;

        mUsed = pStream->readByte();
        if(isPrivate())
        {
            mPublicKey = new Key();
            if(!mPublicKey->readTree(pStream))
                return false;
        }

        unsigned int childCount = pStream->readUnsignedInt();
        Key *newChild;

        mChildLock.lock();
        mChildren.reserve(childCount);
        mChildLock.unlock();

        for(unsigned int i=0;i<childCount;++i)
        {
            newChild = new Key();
            if(!newChild->readTree(pStream))
            {
                delete newChild;
                return false;
            }
            mChildLock.lock();
            mChildren.push_back(newChild);
            mChildLock.unlock();
        }

        return true;
    }

    NextCash::String Key::encode() const
    {
        if(mVersion == MAINNET_PUBKEY_HASH)
            return address();

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        NextCash::Buffer data, checkSum;
        NextCash::String result;

        // Calculate check sum
        write(&digest);
        digest.getResult(&checkSum);

        // Write data and check sum to buffer
        write(&data);
        data.writeStream(&checkSum, 4);

        // Convert to base58
        result.writeBase58(data.begin(), data.length());

        return result;
    }

    bool Key::decode(const char *pText)
    {
        NextCash::Buffer data;

        // Decode base58
        if(data.writeBase58AsBinary(pText) == 0)
            return false;

        // Read into key
        if(!read(&data) || data.remaining() != 4)
        {
            clear();
            return false;
        }

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        NextCash::Buffer checkSum;

        write(&digest);
        digest.getResult(&checkSum);

        checkSum.setInputEndian(NextCash::Endian::BIG);
        if(checkSum.readUnsignedInt() != data.readUnsignedInt())
        {
            clear();
            return false;
        }

        return finalize();
    }

    bool Key::decodePrivateKey(const char *pText)
    {
        clear();

        NextCash::Buffer data;

        // Parse address into public key hash
        data.writeBase58AsBinary(pText);

        if(data.length() != 38)
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_KEY_LOG_NAME,
              "Invalid private key length %d : should be 38", data.length());
            return false;
        }

        AddressType type;
        try
        {
            type = static_cast<AddressType>(data.readByte());
        }
        catch(...)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
              "Invalid address type");
            return false;
        }

        if(type != MAIN_PRIVATE_KEY && type != TEST_PRIVATE_KEY)
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_KEY_LOG_NAME,
              "Invalid private key type 0x%02x", type);
            return false;
        }

        switch(type)
        {
        case MAIN_PRIVATE_KEY:
            mVersion = MAINNET_PRIVATE;
            break;
        case TEST_PRIVATE_KEY:
            mVersion = TESTNET_PRIVATE;
            break;
        default:
            return false;
        }

        mDepth = NO_DEPTH;
        mIndex = NO_DEPTH;
        std::memset(mParentFingerPrint, 0, 4);

        mKey[0] = 0; // Zero for private key
        data.read(mKey + 1, 32);

        // Zeroize chain code
        std::memset(mChainCode, 0, 32);

        uint8_t byte = data.readByte();
        if(byte != 0x01)
        {
            NextCash::Log::addFormatted(NextCash::Log::DEBUG, BITCOIN_KEY_LOG_NAME,
              "Unknown private key sub type 0x%02x : should be 0x01", byte);
            return false;
        }

        uint32_t check = data.readUnsignedInt();

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        data.setReadOffset(0);
        data.readStream(&digest, data.length() - 4);
        NextCash::Buffer checkHash;
        digest.getResult(&checkHash);

        uint32_t checkValue = checkHash.readUnsignedInt();
        if(checkValue != check)
        {
            NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Invalid legacy private key check : 0x%08x != 0x%08x", checkValue, check);
            return false;
        }

        return secp256k1_ec_seckey_verify(context(SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN),
          mKey + 1) && finalize();
    }

    NextCash::String Key::encodePrivateKey()
    {
        NextCash::Buffer data;

        switch(mVersion)
        {
        case MAINNET_PRIVATE:
            data.writeByte(MAIN_PRIVATE_KEY);
            break;
        case TESTNET_PRIVATE:
            data.writeByte(TEST_PRIVATE_KEY);
            break;
        default:
            return NextCash::String();
        }

        data.write(mKey + 1, 32);

        data.writeByte(0x01); // Private key sub type

        NextCash::Digest digest(NextCash::Digest::SHA256_SHA256);
        data.setReadOffset(0);
        data.readStream(&digest, data.length());
        NextCash::Buffer checkHash;
        digest.getResult(&checkHash);

        data.writeUnsignedInt(checkHash.readUnsignedInt());

        data.setReadOffset(0);
        return data.readBase58String(data.length());
    }

    bool Key::finalize()
    {
        if(mPublicKey != NULL)
            delete mPublicKey;

        unsigned int contextFlags = SECP256K1_CONTEXT_VERIFY;
        if(isPrivate())
            contextFlags = SECP256K1_CONTEXT_SIGN;
        secp256k1_context *thisContext = context(contextFlags);
        NextCash::Digest digest(NextCash::Digest::SHA256_RIPEMD160);
        NextCash::Buffer result;

        if(isPrivate())
        {
            mPublicKey = new Key();

            // Create public key
            switch(mVersion)
            {
            case MAINNET_PRIVATE:
                mPublicKey->mVersion = MAINNET_PUBLIC;
                break;
            case TESTNET_PRIVATE:
                mPublicKey->mVersion = TESTNET_PUBLIC;
                break;
            default:
                delete mPublicKey;
                return false;
            }

            mPublicKey->mDepth = mDepth;
            std::memcpy(mPublicKey->mParentFingerPrint, mParentFingerPrint, 4);
            mPublicKey->mIndex = mIndex;
            std::memcpy(mPublicKey->mChainCode, mChainCode, 32);

            secp256k1_pubkey publicKey;
            if(!secp256k1_ec_pubkey_create(thisContext, &publicKey, mKey + 1))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to generate public key for private child key");
                return false;
            }

            size_t compressedLength = 33;
            if(!secp256k1_ec_pubkey_serialize(thisContext, mPublicKey->mKey, &compressedLength,
              &publicKey, SECP256K1_EC_COMPRESSED))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to write compressed public key for private child key");
                return false;
            }

            if(compressedLength != 33)
            {
                NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to write compressed public key for private child key. Invalid return length : %d",
                  compressedLength);
                return false;
            }

            // Calculate hash
            NextCash::Digest hash(NextCash::Digest::SHA256_RIPEMD160);
            hash.write(mPublicKey->mKey, 33);
            hash.getResult(&mPublicKey->mHash);

            digest.write(mPublicKey->mKey, 33);
        }
        else
        {
            mPublicKey = NULL;
            digest.write(mKey, 33);

            // Calculate hash
            NextCash::Digest hash(NextCash::Digest::SHA256_RIPEMD160);
            hash.write(mKey, 33);
            hash.getResult(&mHash);
        }

        digest.getResult(&result);
        result.read(mFingerPrint, 4); // Fingerprint is first 4 bytes of HASH160
        if(mPublicKey != NULL)
            std::memcpy(mPublicKey->mFingerPrint, mFingerPrint, 4);
        return true;
    }

    Key *Key::findAddress(const NextCash::Hash &pHash)
    {
        if(pHash == hash())
            return this;

        mChildLock.lock();
        Key *result;
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
        {
            result = (*child)->findAddress(pHash);
            if(result != NULL)
            {
                mChildLock.unlock();
                return result;
            }
        }
        mChildLock.unlock();

        return NULL;
    }

    Key *Key::chainKey(uint32_t pChain, DerivationPathMethod pMethod, uint32_t pAccount,
      uint32_t pCoin)
    {
        switch(pMethod)
        {
        case SIMPLE: // m/account/chain
        {
            if(mDepth != 0) // Master key
                return NULL;
            return deriveChild(pChain);
        }
        case BIP0032: // m/account/chain
        {
            if(pAccount == 0xffffffff)
                pAccount = HARDENED; // Default

            Key *account = NULL;
            if(mDepth == 0) // Master key
                account = deriveChild(pAccount);
            if(account == NULL)
                return NULL;

            return account->deriveChild(pChain);
        }
        case BIP0044: // m/44'/coin/account/chain
        {
            // Purpose
            Key *purpose = NULL;
            if(mDepth == 0) // Master key
                purpose = deriveChild(HARDENED + 44);
            if(purpose == NULL)
                return NULL;

            // Coin
            if(pCoin == 0xffffffff)
                pCoin = COIN_BITCOIN; // Default
            Key *coin = purpose->deriveChild(pCoin);
            if(coin == NULL)
                return NULL;

            // Account
            if(pAccount == 0xffffffff)
                pAccount = HARDENED; // Default
            Key *account = NULL;
            account = coin->deriveChild(pAccount);
            if(account == NULL)
                return NULL;

            return account->deriveChild(pChain);
        }
        default:
            return NULL;
        }
    }

    bool Key::updateGap(unsigned int pGap)
    {
        if(mDepth == NO_DEPTH)
            return false;

        unsigned int gap = 0;
        unsigned int nextIndex = 0;

        mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
        {
            if((*child)->mIndex >= nextIndex)
                nextIndex = (*child)->mIndex + 1;
            if((*child)->mUsed)
                gap = 0;
            else
                ++gap;
        }
        mChildLock.unlock();

        if(gap < pGap)
        {
            while(gap < pGap)
                if(deriveChild(nextIndex) != NULL)
                {
                    ++gap;
                    ++nextIndex;
                }
            return true;
        }
        else
            return false;
    }

    Key *Key::markUsed(const NextCash::Hash &pHash, unsigned int pGap, bool &pNewAddresses)
    {
        if(hash() == pHash)
        {
            if(mUsed)
            {
                // Already used
                pNewAddresses = false;
                return this;
            }

            // Mark as used
            // The parent is apparently not available, so no new addresses will be generated.
            pNewAddresses = false;
            mUsed = true;
            if(mPublicKey != NULL)
                mPublicKey->mUsed = true;
            return this;
        }

        pNewAddresses = false;
        Key *result = NULL;
        unsigned int gap = 0;
        unsigned int lastIndex = 0;

        mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
        {
            if(result != NULL)
            {
                lastIndex = (*child)->mIndex;
                if((*child)->mUsed)
                    gap = 0;
                else
                    ++gap;
            }
            else if((*child)->hash() == pHash)
            {
                lastIndex = (*child)->mIndex;
                result = *child;

                if(result->mUsed)
                {
                    mChildLock.unlock();
                    return result; // Already used so no new addresses will be needed
                }

                result->mUsed = true;
                if(result->mPublicKey != NULL)
                    result->mPublicKey->mUsed = true;
            }
            else
            {
                result = (*child)->markUsed(pHash, pGap, pNewAddresses);
                if(result != NULL)
                {
                    mChildLock.unlock();
                    return result;
                }
            }
        }
        mChildLock.unlock();

        // Check if more addresses need to be generated
        if(result != NULL && gap < pGap)
        {
            // TODO Add support for after 2^31 indices are used up
            pNewAddresses = true;
            ++lastIndex; // Go to next index
            while(gap < pGap)
                if(deriveChild(lastIndex) != NULL)
                {
                    ++gap;
                    ++lastIndex;
                }
        }

        return result;
    }

    bool Key::synchronize(Key *pOther)
    {
        if(pOther->hash() == hash())
        {
            if(pOther->mUsed)
            {
                mUsed = true;
                if(mPublicKey != NULL)
                    mPublicKey->mUsed = true;
            }

            Key *childKey;
            mChildLock.lock();
            for(std::vector<Key *>::iterator otherChild = pOther->mChildren.begin();
              otherChild != pOther->mChildren.end(); ++otherChild)
            {
                // Derive or find child with matching index to synchronize
                childKey = deriveChild((*otherChild)->index(), true);
                if(childKey == NULL)
                {
                    mChildLock.unlock();
                    return false;
                }
                childKey->synchronize(*otherChild);
            }
            mChildLock.unlock();

            return true;
        }
        else
        {
            // Check if pOther is below this node in the hierarchy
            mChildLock.lock();
            for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
              ++child)
                if((*child)->synchronize(pOther))
                {
                    mChildLock.unlock();
                    return true;
                }
            mChildLock.unlock();
        }

        return false;
    }

    Key *Key::getNextUnused()
    {
        if(mDepth == NO_DEPTH)
            return this;

        mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
            if(!(*child)->mUsed)
            {
                mChildLock.unlock();
                return *child;
            }
        mChildLock.unlock();
        return NULL;
    }

    void Key::getChildren(std::vector<Key *> &pChildren)
    {
        pChildren.clear();
        pChildren.reserve(mChildren.size());
        mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
            pChildren.push_back(*child);
        mChildLock.unlock();
    }

    Key *Key::findChild(uint32_t pIndex, bool pLocked)
    {
        if(!pLocked)
            mChildLock.lock();
        for(std::vector<Key *>::iterator child = mChildren.begin(); child != mChildren.end();
          ++child)
            if((*child)->index() == pIndex)
            {
                if(!pLocked)
                    mChildLock.unlock();
                return *child;
            }
        if(!pLocked)
            mChildLock.unlock();
        return NULL;
    }

    bool Key::sign(const NextCash::Hash &pHash, Signature &pSignature) const
    {
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_KEY_SIGN_ID, PROFILER_KEY_SIGN_NAME), true);
#endif
        if(!isPrivate())
            return false;

        if(pHash.size() != 32)
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Wrong size hash to sign");
            return false;
        }

        secp256k1_ecdsa_signature signature;
        if(!secp256k1_ecdsa_sign(context(SECP256K1_CONTEXT_SIGN), &signature, pHash.data(),
          mKey + 1, secp256k1_nonce_function_default, NULL))
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME, "Failed to sign hash");
            return false;
        }

        pSignature.set(signature.data);
        return true;
    }

    bool Key::verify(const Signature &pSignature, const NextCash::Hash &pHash) const
    {
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_KEY_VERIFY_SIG_ID, PROFILER_KEY_VERIFY_SIG_NAME), true);
#endif
        if(isPrivate())
        {
            if(mPublicKey == NULL)
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME, "Invalid key");
                return false;
            }
            else
                return mPublicKey->verify(pSignature, pHash);
        }

        if(pHash.size() != SIGNATURE_HASH_SIZE)
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Wrong size hash to verify");
            return false;
        }

        secp256k1_context *thisContext = context(SECP256K1_CONTEXT_VERIFY);
        secp256k1_pubkey publicKey;
        if(!secp256k1_ec_pubkey_parse(thisContext, &publicKey, mKey, 33))
        {
            NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
              "Failed to parse public key");
            return false;
        }

        if(secp256k1_ecdsa_verify(thisContext, (const secp256k1_ecdsa_signature *)pSignature.data(),
          pHash.data(), &publicKey))
            return true;

        if(!secp256k1_ecdsa_signature_normalize(thisContext,
          (secp256k1_ecdsa_signature *)pSignature.data(),
          (const secp256k1_ecdsa_signature *)pSignature.data()))
            return false; // Already normalized

        // Try it again with the normalized signature
        if(secp256k1_ecdsa_verify(thisContext, (const secp256k1_ecdsa_signature *)pSignature.data(),
          pHash.data(), &publicKey))
            return true;

        return false;
    }

    bool Key::verify(const uint8_t *pPublicKeyData, unsigned int pPublicKeyDataSize,
      const uint8_t *pSignatureData, unsigned int pSignatureDataSize, bool pStrictSignatures,
      const NextCash::Hash &pHash)
    {
#ifdef PROFILER_ON
        NextCash::ProfilerReference profiler(NextCash::getProfiler(PROFILER_SET,
          PROFILER_KEY_STATIC_VERIFY_SIG_ID, PROFILER_KEY_STATIC_VERIFY_SIG_NAME), true);
#endif

        if(pHash.size() != SIGNATURE_HASH_SIZE)
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
              "Wrong size hash to verify");
            return false;
        }

        // Parse public key data.
        secp256k1_context *thisContext = context(SECP256K1_CONTEXT_VERIFY);
        secp256k1_pubkey publicKey;
        if(!secp256k1_ec_pubkey_parse(thisContext, &publicKey, pPublicKeyData,
          pPublicKeyDataSize))
        {
            NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
              "Failed to parse public key");
            return false;
        }

        // Parse signature data
        secp256k1_ecdsa_signature signature;
        if(pStrictSignatures)
        {
            if(!secp256k1_ecdsa_signature_parse_der(thisContext, &signature, pSignatureData,
              pSignatureDataSize))
            {
                NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
                  "Failed to parse signature");
                return false;
            }
        }
        else
        {
            // Max of 2 bytes added by repair.
            uint8_t repairedSignature[pSignatureDataSize + 2];
            std::memcpy(repairedSignature, pSignatureData, pSignatureDataSize);

            repairSignature(repairedSignature, pSignatureDataSize);

            if(!secp256k1_ecdsa_signature_parse_der(thisContext, &signature,
              repairedSignature, pSignatureDataSize))
            {
                NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
                  "Failed to parse repaired signature");
                return false;
            }
        }

        // Verify signature
        if(secp256k1_ecdsa_verify(thisContext, &signature, pHash.data(), &publicKey))
            return true;

        // Normalize and attempt verify again if it wasn't normalized.
        if(secp256k1_ecdsa_signature_normalize(thisContext, &signature, &signature) &&
          secp256k1_ecdsa_verify(thisContext, &signature, pHash.data(), &publicKey))
            return true;

        NextCash::String hex;
        hex.writeHex(pSignatureData, pSignatureDataSize);
        NextCash::Log::addFormatted(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
          "Failed signature verify (%d bytes) : %s", pSignatureDataSize, hex.text());
        return false;
    }

    Key *Key::deriveChild(uint32_t pIndex, bool pLocked)
    {
        Key *result = findChild(pIndex, pLocked);

        if(result != NULL)
            return result; // Already created

        if(mVersion == EMPTY || mDepth >= 100)
            return NULL;

        secp256k1_context *thisContext = context(SECP256K1_CONTEXT_SIGN);
        NextCash::HMACDigest hmac(NextCash::Digest::SHA512);
        NextCash::Buffer hmacKey, hmacResult;

        if(isPrivate())
        {
            result = new Key();

            switch(mVersion)
            {
            case MAINNET_PRIVATE:
                result->mVersion = MAINNET_PRIVATE;
                break;
            case TESTNET_PRIVATE:
                result->mVersion = TESTNET_PRIVATE;
                break;
            default:
                delete result;
                return NULL;
            }

            result->mDepth = mDepth + 1;
            std::memcpy(result->mParentFingerPrint, mFingerPrint, 4);
            result->mIndex = pIndex;

            hmacKey.write(chainCode(), 32);
            hmac.setOutputEndian(NextCash::Endian::BIG);
            hmac.initialize(&hmacKey);

            if(pIndex >= HARDENED)
            {
                // Index >= 2^31 - Hardened child
                // I = HMAC-SHA512(Key = cpar, Data = 0x00 || ser256(kpar) || ser32(i))
                // Leading zero byte already in private key data
                hmac.write(mKey, 33); // 0x00 || ser256(kpar)
            }
            else
            {
                // Index < 2^31
                // I = HMAC-SHA512(Key = cpar, Data = serP(point(kpar)) || ser32(i))
                hmac.write(mPublicKey->mKey, 33); // serP(point(kpar))
            }

            hmac.writeUnsignedInt(pIndex); // ser32(i)
            hmac.getResult(&hmacResult);

            // Split I into two 32-byte sequences, IL and IR.

            // The returned child key ki is parse256(IL) + kpar (mod n).
            uint8_t tweak[32];
            hmacResult.read(tweak, 32);
            result->mKey[0] = 0;
            std::memcpy(result->mKey + 1, key() + 1, 32);

            if(!secp256k1_ec_privkey_tweak_add(thisContext, result->mKey + 1, tweak))
            {
                delete result;
                return NULL;
            }

            // In case parse256(IL) ≥ n or ki = 0, the resulting key is invalid, and one should proceed
            //   with the next value for i. (Note: this has probability lower than 1 in 2127.)
            if(!secp256k1_ec_seckey_verify(thisContext, result->mKey + 1))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to generate valid private child key");
                delete result;
                return NULL;
            }
        }
        else // Public
        {
            if(pIndex >= HARDENED)
                return NULL;

            result = new Key();

            switch(mVersion)
            {
            case MAINNET_PRIVATE:
            case MAINNET_PUBLIC:
                result->mVersion = MAINNET_PUBLIC;
                break;
            case TESTNET_PRIVATE:
            case TESTNET_PUBLIC:
                result->mVersion = TESTNET_PUBLIC;
                break;
            default:
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Invalid parent version for derive");
                delete result;
                return NULL;
            }

            result->mDepth = mDepth + 1;
            std::memcpy(result->mParentFingerPrint, mFingerPrint, 4);
            result->mIndex = pIndex;

            // I = HMAC-SHA512(Key = cpar, Data = serP(Kpar) || ser32(i))
            hmacKey.write(chainCode(), 32); // Key = cpar
            hmac.setOutputEndian(NextCash::Endian::BIG);
            hmac.initialize(&hmacKey);

            hmac.write(mKey, 33);
            hmac.writeUnsignedInt(pIndex);
            hmac.getResult(&hmacResult);

            // Split I into two 32-byte sequences, IL and IR.

            // The returned child key Ki is point(parse256(IL)) + Kpar.

            hmacResult.read(result->mKey + 1, 32);

            // In case parse256(IL) ≥ n or Ki is the point at infinity, the resulting key is invalid,
            //   and one should proceed with the next value for i.
            if(!secp256k1_ec_seckey_verify(thisContext, result->mKey + 1))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to generate valid private key for public child key");
                delete result;
                return NULL;
            }

            // Create public key for new private key
            secp256k1_pubkey *publicKeys[2];
            publicKeys[0] = new secp256k1_pubkey();
            if(!secp256k1_ec_pubkey_create(thisContext, publicKeys[0], result->mKey + 1))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to generate public key for public child key");
                delete publicKeys[0];
                delete result;
                return NULL;
            }

            // Parse parent public key to uncompressed format
            publicKeys[1] = new secp256k1_pubkey();
            if(!secp256k1_ec_pubkey_parse(thisContext, publicKeys[1], mKey, 33))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to parse KeyTree Key public key");
                delete publicKeys[0];
                delete publicKeys[1];
                delete result;
                return NULL;
            }

            // Combine generated public key and parent public key into new child key
            secp256k1_pubkey newPublicKey;
            if(!secp256k1_ec_pubkey_combine(thisContext, &newPublicKey,
              (const secp256k1_pubkey * const *)publicKeys, 2))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to combine public keys");
                delete result;
                return NULL;
            }

            delete publicKeys[0];
            delete publicKeys[1];

            size_t compressedLength = 33;
            if(!secp256k1_ec_pubkey_serialize(thisContext, result->mKey, &compressedLength,
              &newPublicKey, SECP256K1_EC_COMPRESSED))
            {
                NextCash::Log::add(NextCash::Log::VERBOSE, BITCOIN_KEY_LOG_NAME,
                  "Failed to write compressed public key for public child key");
                delete result;
                return NULL;
            }
        }

        // The returned chain code ci is IR.
        hmacResult.read(result->mChainCode, 32);

        if(result->finalize())
        {
            if(!pLocked)
                mChildLock.lock();
            mChildren.push_back(result);
            if(!pLocked)
                mChildLock.unlock();
            return result;
        }
        else
        {
            delete result;
            return NULL;
        }
    }

    Key *Key::derivePath(const std::vector<uint32_t> &pPath)
    {
        Key *result = this;
        for(std::vector<uint32_t>::const_iterator index = pPath.begin(); index != pPath.end(); ++index)
        {
            result = result->deriveChild(*index);
            if(result == NULL)
                return NULL;
        }
        return result;
    }

    bool Key::loadBinarySeed(Network pNetwork, NextCash::InputStream *pStream)
    {
        clear();

        switch(pNetwork)
        {
        case MAINNET:
            mVersion = MAINNET_PRIVATE;
            break;
        case TESTNET:
            mVersion = TESTNET_PRIVATE;
            break;
        default:
            return false;
        }

        mDepth = 0;
        std::memset(mParentFingerPrint, 0, 4);
        mIndex = 0;

        NextCash::HMACDigest hmac(NextCash::Digest::SHA512);
        NextCash::Buffer hmacKey, hmacResult;

        // Calculate HMAC SHA512
        hmacKey.writeString("Bitcoin seed");
        hmac.initialize(&hmacKey);
        hmac.writeStream(pStream, pStream->length());
        hmac.getResult(&hmacResult);

        // Split HMAC SHA512 into halves for key and chain code
        mKey[0] = 0; // Zero for private key
        hmacResult.read(mKey + 1, 32);
        hmacResult.read(mChainCode, 32);

        return secp256k1_ec_seckey_verify(context(SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN),
          mKey + 1) && finalize();
    }

    NextCash::String createMnemonicFromSeed(Mnemonic::Language pLanguage,
      NextCash::InputStream *pSeed)
    {
        NextCash::String result;
        NextCash::Digest digest(NextCash::Digest::SHA256);
        NextCash::Buffer checkSum;
        std::vector<bool> bits;
        unsigned int nextByte, bitMask;

        // Calculate checksum
        pSeed->setReadOffset(0);
        digest.writeStream(pSeed, pSeed->length());
        digest.getResult(&checkSum);

        int checkSumBits = pSeed->length() / 4; // Entropy bit count / 32

        // Copy seed to bit vector
        pSeed->setReadOffset(0);
        while(pSeed->remaining())
        {
            nextByte = pSeed->readByte();
            for(bitMask = 0x01 << 7; bitMask != 0; bitMask >>= 1)
                bits.push_back((nextByte & bitMask) != 0);
        }

        // Append check sum
        while(checkSumBits > 0)
        {
            nextByte = checkSum.readByte();
            for(bitMask = 0x01 << 7; bitMask != 0 && checkSumBits > 0;
              bitMask >>= 1, --checkSumBits)
                bits.push_back((nextByte & bitMask) != 0);
        }

        // Parse 11 bits at a time and add words to the sentence
        uint16_t value = 0;
        int valueBits = 0;
        for(std::vector<bool>::iterator bit = bits.begin(); bit != bits.end(); ++bit)
        {
            ++valueBits;
            value <<= 1;
            if(*bit)
                value |= 0x01;

            if(valueBits == 11)
            {
                // Add word
                if(result.length() > 0)
                    result += ' ';
                result += Mnemonic::WORDS[pLanguage][value];

                valueBits = 0;
                value = 0;
            }
        }

        if(valueBits > 0)
        {
            // Add word
            if(result.length() > 0)
                result += ' ';
            result += Mnemonic::WORDS[pLanguage][value];

            valueBits = 0;
            value = 0;
        }

        return result;
    }

    NextCash::String Key::generateMnemonicSeed(Mnemonic::Language pLanguage, unsigned int pBytesEntropy)
    {
        // Generate specified number of bytes of entropy
        NextCash::Buffer seed;
        for(unsigned int i=0;i<pBytesEntropy;i+=4)
            seed.writeUnsignedInt(NextCash::Math::randomInt());
        return createMnemonicFromSeed(pLanguage, &seed);
    }

    bool Key::validateMnemonicSeed(const char *pText, const char *pPassPhrase, const char *pSalt)
    {
        std::vector<bool> bits;
        const char *ptr;
        const char **checkWord;
        NextCash::String word;
        bool found;
        unsigned int value;

        // Loop through languages
        for(unsigned int languageIndex = 0; languageIndex < Mnemonic::LANGUAGE_COUNT;
          ++languageIndex)
        {
            // Parse words from text
            bits.clear();
            ptr = pText;
            while(*ptr)
            {
                if(*ptr == ' ' && word.length())
                {
                    // Lookup word in mnemonics and add value to list
                    // TODO Implement binary search
                    found = false;
                    checkWord = Mnemonic::WORDS[languageIndex];
                    for(value = 0; value < Mnemonic::WORD_COUNT; ++value, ++checkWord)
                        if(word == *checkWord)
                        {
                            for(unsigned int mask = 0x01 << 10; mask != 0; mask >>= 1)
                                bits.push_back((value & mask) != 0);
                            found = true;
                            break;
                        }

                    word.clear();

                    if(!found)
                        break;
                }
                else
                    word += NextCash::lower(*ptr);

                ++ptr;
            }

            if(!found)
                continue; // Next language

            if(word.length())
            {
                found = false;
                checkWord = Mnemonic::WORDS[languageIndex];
                for(value = 0; value < Mnemonic::WORD_COUNT; ++value, ++checkWord)
                    if(word == *checkWord)
                    {
                        for(unsigned int mask = 0x01 << 10; mask != 0; mask >>= 1)
                            bits.push_back((value & mask) != 0);
                        found = true;
                        break;
                    }

                if(!found)
                    continue; // Next language
            }
        }

        // Check if values is a valid seed
        if(bits.size() < 128)
            return false;

        int checkSumBits = 0, seedBits = 0;
        for(unsigned int i = 128; i <= 256; i += 32)
            if(bits.size() == i + (i / 32))
            {
                seedBits = i;
                checkSumBits = i / 32;
                break;
            }

        if(checkSumBits == 0)
            return false;

        // Parse bits
        NextCash::Buffer seedData, checkSumData;
        unsigned int valueBits = 0;
        unsigned int valueMask = 0x01 << 7;

        value = 0;

        for(std::vector<bool>::iterator bit = bits.begin(); bit != bits.end(); ++bit)
        {
            --seedBits;
            ++valueBits;
            if(*bit)
                value |= valueMask;
            valueMask >>= 1;

            if(valueBits == 8)
            {
                if(seedBits >= 0)
                    seedData.writeByte(value);
                else
                    checkSumData.writeByte(value);
                value = 0;
                valueBits = 0;
                valueMask = 0x01 << 7;
            }
        }

        if(valueBits > 0)
        {
            //value <<= (8 - valueBits);
            if(seedBits >= 0)
                seedData.writeByte(value);
            else
                checkSumData.writeByte(value);
        }

        // Calculate checksum
        NextCash::Digest digest(NextCash::Digest::SHA256);
        NextCash::Buffer checkSum;

        seedData.setReadOffset(0);
        digest.writeStream(&seedData, seedData.length());
        checkSum.clear();
        digest.getResult(&checkSum);

        // Verify checksum
        bool matches = true;
        for(int bit = checkSumBits; bit > 0; bit -= 8)
        {
            if(bit >= 8)
            {
                if(checkSum.readByte() != checkSumData.readByte())
                {
                    matches = false;
                    break;
                }
            }
            else if((checkSum.readByte() >> bit) != (checkSumData.readByte() >> bit))
            {
                matches = false;
                break;
            }
        }

        return matches;
    }

    // PBKDF2 with HMAC SHA512, 2048 iterations, and output length of 512 bits.
    bool processMnemonicSeed(NextCash::InputStream *pMnemonicSentence,
      NextCash::InputStream *pSaltPlusPassPhrase, NextCash::OutputStream *pResult)
    {
        NextCash::HMACDigest digest(NextCash::Digest::SHA512);
        NextCash::Buffer dataList[2], *data, *round, result;

        data = dataList;
        round = dataList + 1;

        // Write salt, passphrase, and iteration index into data for first round
        data->setOutputEndian(NextCash::Endian::BIG);
        pSaltPlusPassPhrase->setReadOffset(0);
        data->writeStream(pSaltPlusPassPhrase, pSaltPlusPassPhrase->length());
        data->writeUnsignedInt(1); // Iteration index (only 1 iteration since output length is 512)

        // Initialize result to zeros
        for(unsigned int i=0;i<64;++i)
            result.writeByte(0);

        for(unsigned int i=0;i<2048;++i)
        {
            // Calculate HMAC SHA512
            pMnemonicSentence->setReadOffset(0);
            digest.initialize(pMnemonicSentence);

            data->setReadOffset(0);
            digest.writeStream(data, data->length());

            round->setWriteOffset(0);
            digest.getResult(round);

            // Xor round into result
            round->setReadOffset(0);
            result.setReadOffset(0);
            result.setWriteOffset(0);
            while(result.remaining())
                result.writeByte(result.readByte() ^ round->readByte());

            // Swap data and round
            if(data == dataList)
            {
                data = dataList + 1;
                round = dataList;
            }
            else
            {
                data = dataList;
                round = dataList + 1;
            }
        }

        result.setReadOffset(0);
        pResult->writeStream(&result, result.length());

        // Zeroize before releasing memory since a password might have been in here
        data->zeroize();
        round->zeroize();
        result.zeroize();
        return true;
    }

    bool Key::loadMnemonicSeed(Network pNetwork, const char *pMnemonicSentence,
      const char *pPassPhrase, const char *pSalt)
    {
        clear();

        NextCash::Buffer sentence, salt, seed;
        sentence.writeString(pMnemonicSentence);
        salt.writeString(pSalt);
        salt.writeString(pPassPhrase);
        if(!processMnemonicSeed(&sentence, &salt, &seed))
            return false;

        return loadBinarySeed(pNetwork, &seed);
    }

    class PublicKeyData
    {
    public:

        PublicKeyData()
        {
            hasPrivate = false;
            derivationPathMethod = Key::DERIVE_UNKNOWN;
            flags = 0;
            createdDate = 0;
            gap = Key::DEFAULT_GAP;
        }
        ~PublicKeyData()
        {
            for(std::vector<Key *>::iterator key = chainKeys.begin(); key != chainKeys.end(); ++key)
                delete *key;
        }

        void write(NextCash::OutputStream *pStream) const
        {
            if(hasPrivate)
                pStream->writeByte(0xff);
            else
                pStream->writeByte(0);
            pStream->writeUnsignedInt(name.length());
            pStream->writeString(name);
            pStream->writeUnsignedInt(flags);

            pStream->writeByte(derivationPathMethod);
            pStream->writeUnsignedInt(createdDate);
            pStream->writeUnsignedInt(gap);

            pStream->writeUnsignedInt(chainKeys.size());
            std::vector<std::vector<uint32_t>>::const_iterator path = chainKeyPaths.begin();
            for(std::vector<Key *>::const_iterator key = chainKeys.begin(); key != chainKeys.end();
              ++key, ++path)
            {
                (*key)->writeTree(pStream);

                // Write path integers
                pStream->writeUnsignedInt(path->size());
                for(std::vector<uint32_t>::const_iterator index = path->begin();
                  index != path->end(); ++index)
                    pStream->writeUnsignedInt(*index);
            }
        }

        bool read(NextCash::InputStream *pStream, unsigned int pVersion)
        {
            if(pVersion == 1)
                pStream->readUnsignedInt();

            hasPrivate = pStream->readByte() != 0;

            unsigned int nameLength = pStream->readUnsignedInt();
            name = pStream->readString(nameLength);

            flags = pStream->readUnsignedInt();

            try
            {
                derivationPathMethod = static_cast<Key::DerivationPathMethod>(pStream->readByte());
            }
            catch(...)
            {
                NextCash::Log::add(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
                  "Invalid derivation path method");
                return false;
            }

            if(pVersion > 1)
                createdDate = pStream->readUnsignedInt();
            else
            {
                createdDate = 0;
                flags |= PASS_STARTED;
            }

            if(pVersion > 3)
                gap = pStream->readUnsignedInt();
            else
                gap = Key::DEFAULT_GAP;

            unsigned int chainCount = pStream->readUnsignedInt();
            Key *newKey;

            chainKeys.clear();
            chainKeys.reserve(chainCount);
            for(unsigned int i = 0; i < chainCount; ++i)
            {
                newKey = new Key();
                if(newKey->readTree(pStream))
                {
                    chainKeys.push_back(newKey);
                    chainKeyPaths.emplace_back();
                    std::vector<uint32_t> &path = chainKeyPaths.back();

                    if(pVersion > 2)
                    {
                        // Read path integers
                        unsigned int pathCount = pStream->readUnsignedInt();
                        path.reserve(pathCount);

                        for(unsigned int j = 0; j < pathCount; ++j)
                            path.emplace_back(pStream->readUnsignedInt());
                    }
                    else
                    {
                        // Default BIP-0044 path.
                        path.emplace_back(Key::PURPOSE_44);
                        path.emplace_back(Key::COIN_BITCOIN);
                        path.emplace_back(Key::HARDENED);
                        path.emplace_back(i); // 0 Receiving, 1 Change
                    }
                }
                else
                {
                    delete newKey;
                    return false;
                }
            }

            return true;
        }

        bool addChainKeys(Key *pKey, Key::DerivationPathMethod pMethod, uint32_t pCoinIndex,
          uint32_t pIndex);

        bool hasPrivate;
        NextCash::String name;
        Key::DerivationPathMethod derivationPathMethod;
        Time createdDate;
        uint32_t gap;
        uint32_t flags;

        // Flag values
        static const uint32_t SYNCHRONIZED = 0x01;
        static const uint32_t BACKED_UP    = 0x02;
        static const uint32_t PASS_STARTED = 0x04;

        // Public keys used to generate addresses without access to private keys
        std::vector<Key *> chainKeys;
        std::vector<std::vector<uint32_t>> chainKeyPaths;

    private:
        PublicKeyData(const PublicKeyData &pCopy);
        PublicKeyData &operator = (const PublicKeyData &pRight);
    };

    class PrivateKeyData
    {
    public:

        PrivateKeyData()
        {
            key = new Key();
        }
        PrivateKeyData(Key *pKey)
        {
            key = pKey;
        }
        PrivateKeyData(const PrivateKeyData &pCopy)
        {
            seed = pCopy.seed;
            if(key != NULL)
                delete key;
            key = new Key(*pCopy.key);
        }
        PrivateKeyData &operator = (const PrivateKeyData &pRight)
        {
            seed = pRight.seed;
            if(key != NULL)
                delete key;
            key = new Key(*pRight.key);
            return *this;
        }
        ~PrivateKeyData()
        {
            if(key != NULL)
                delete key;
            seed.clean();
        }

        void write(NextCash::OutputStream *pStream) const
        {
            pStream->writeUnsignedInt(seed.length());
            pStream->writeString(seed);

            key->writeTree(pStream);
        }

        bool read(NextCash::InputStream *pStream, unsigned int pVersion)
        {
            unsigned int seedLength = pStream->readUnsignedInt();
            seed = pStream->readString(seedLength);

            key = new Key();
            if(!key->readTree(pStream))
            {
                key = NULL;
                return false;
            }

            return true;
        }

        NextCash::String seed;
        Key *key;
    };

    bool PublicKeyData::addChainKeys(Key *pKey, Key::DerivationPathMethod pMethod,
      uint32_t pCoinIndex, uint32_t pIndex)
    {
        Key *chain;
        switch(pMethod)
        {
        case Key::INDIVIDUAL:
            // Add only address key as chain key
            chainKeys.push_back(new Key(*pKey->publicKey()));
            chainKeyPaths.emplace_back();
            chainKeyPaths.back().emplace_back(pIndex);
            return true;

        case Key::SIMPLE:
            chain = pKey->chainKey(pIndex, Key::SIMPLE, 0, 0);
            if(chain != NULL)
            {
                if(chain->isPrivate())
                    chainKeys.push_back(new Key(*chain->publicKey()));
                else
                    chainKeys.push_back(new Key(*chain));

                // Specify Path
                chainKeyPaths.emplace_back();
                chainKeyPaths.back().emplace_back(pIndex);
                return true;
            }
            else
                return false;

        case Key::BIP0032:
            // Receiving chain
            chain = pKey->chainKey(pIndex, Key::BIP0032, 0, 0);
            if(chain != NULL)
            {
                if(chain->isPrivate())
                    chainKeys.push_back(new Key(*chain->publicKey()));
                else
                    chainKeys.push_back(new Key(*chain));

                // Specify Path
                chainKeyPaths.emplace_back();
                chainKeyPaths.back().emplace_back(Key::HARDENED);
                chainKeyPaths.back().emplace_back(pIndex);
                return true;
            }
            else
                return false;

        case Key::BIP0044:
            // Receiving chain
            chain = pKey->chainKey(pIndex, Key::BIP0044, Key::HARDENED, pCoinIndex);
            if(chain != NULL)
            {
                if(chain->isPrivate())
                    chainKeys.push_back(new Key(*chain->publicKey()));
                else
                    chainKeys.push_back(new Key(*chain));

                // Specify Path 44'/Coin/Account/Chain
                chainKeyPaths.emplace_back();
                chainKeyPaths.back().emplace_back(Key::HARDENED + 44);
                chainKeyPaths.back().emplace_back(pCoinIndex);
                chainKeyPaths.back().emplace_back(Key::HARDENED);
                chainKeyPaths.back().emplace_back(pIndex);
                return true;
            }
            else
                return false;

        default:
        case Key::DERIVE_CUSTOM:
        case Key::DERIVE_UNKNOWN:
            return false;
        }
    }

    static const char sEncryptKeyInitVector[] = "0daf9958eec1c536d8bed3608942b560";

    KeyStore::KeyStore()
    {
        mLoaded = false;
        mPrivateLoaded = true;
    }

    KeyStore::~KeyStore()
    {
        for(std::vector<PublicKeyData *>::iterator key=mKeys.begin();key!=mKeys.end();++key)
            delete *key;
        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            delete *key;
    }

    bool KeyStore::allAreSynchronized()
    {
        bool result = true;
        for(std::vector<PublicKeyData *>::iterator key = mKeys.begin(); key != mKeys.end(); ++key)
            if(((*key)->flags & PublicKeyData::SYNCHRONIZED) == 0)
                result = false;
        return result;
    }

    void KeyStore::setAllSynchronized()
    {
        for(std::vector<PublicKeyData *>::iterator key = mKeys.begin(); key != mKeys.end(); ++key)
            if(((*key)->flags & PublicKeyData::PASS_STARTED) != 0)
                (*key)->flags |= PublicKeyData::SYNCHRONIZED;
    }

    bool KeyStore::allPassesStarted()
    {
        bool result = true;
        for(std::vector<PublicKeyData *>::iterator key = mKeys.begin(); key != mKeys.end(); ++key)
            if(((*key)->flags & PublicKeyData::PASS_STARTED) == 0)
                result = false;
        return result;
    }

    void KeyStore::setAllPassStarted()
    {
        for(std::vector<PublicKeyData *>::iterator key = mKeys.begin(); key != mKeys.end(); ++key)
            (*key)->flags |= PublicKeyData::PASS_STARTED;
    }

    bool KeyStore::hasPrivate(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return false;

        return mKeys[pOffset]->hasPrivate;
    }

    NextCash::String KeyStore::name(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return NextCash::String();

        return mKeys[pOffset]->name;
    }

    bool KeyStore::isSynchronized(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return NextCash::String();

        return (mKeys[pOffset]->flags & PublicKeyData::SYNCHRONIZED) != 0;
    }

    bool KeyStore::isBackedUp(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return NextCash::String();

        return (mKeys[pOffset]->flags & PublicKeyData::BACKED_UP) != 0;
    }

    Key::DerivationPathMethod KeyStore::derivationPathMethod(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return Key::DERIVE_UNKNOWN;

        return mKeys[pOffset]->derivationPathMethod;
    }

    void KeyStore::getDerivationPath(unsigned int pOffset, unsigned int pChainOffset,
      std::vector<uint32_t> &pPath)
    {
        pPath.clear();

        if(pOffset >= mKeys.size() || pChainOffset >= mKeys[pOffset]->chainKeyPaths.size())
            return;

        pPath = mKeys[pOffset]->chainKeyPaths[pChainOffset];
    }

    Time KeyStore::createdDate(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return 0;

        return mKeys[pOffset]->createdDate;
    }

    unsigned int KeyStore::gap(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return 0;

        return mKeys[pOffset]->gap;
    }

    std::vector<Key *> *KeyStore::chainKeys(unsigned int pOffset)
    {
        if(pOffset >= mKeys.size())
            return NULL;

        return &mKeys[pOffset]->chainKeys;
    }

    Key *KeyStore::chainKey(unsigned int pOffset, uint32_t pIndex)
    {
        if(pOffset >= mKeys.size())
            return NULL;

        std::vector<Key *> &chainKeys = mKeys[pOffset]->chainKeys;
        for(std::vector<Key *>::iterator key = chainKeys.begin(); key != chainKeys.end(); ++key)
            if((*key)->index() == pIndex)
                return *key;

        // For individual keys that don't have an indices and depth.
        if(chainKeys.size() == 1 && chainKeys.front()->depth() == Key::NO_DEPTH)
            return chainKeys.front();

        return NULL;
    }

    NextCash::String KeyStore::seed(unsigned int pOffset)
    {
        if(!mPrivateLoaded || pOffset >= mPrivateKeys.size())
            return NextCash::String();

        return mPrivateKeys[pOffset]->seed;
    }

    bool KeyStore::passStarted(unsigned int pOffset)
    {
        return (mKeys[pOffset]->flags & PublicKeyData::PASS_STARTED) != 0;
    }

    Key *KeyStore::fullKey(unsigned int pOffset)
    {
        if(!mPrivateLoaded || pOffset >= mPrivateKeys.size() || !synchronize(pOffset))
            return NULL;

        return mPrivateKeys[pOffset]->key;
    }

    bool KeyStore::synchronize(unsigned int pOffset)
    {
        if(!mPrivateLoaded || pOffset >= mPrivateKeys.size())
            return false;

        std::vector<Key *> &chainKeys = mKeys[pOffset]->chainKeys;
        for(std::vector<Key *>::iterator key = chainKeys.begin();
          key != chainKeys.end(); ++key)
            mPrivateKeys[pOffset]->key->synchronize(*key);

        return true;
    }

    void KeyStore::setName(unsigned int pOffset, const char *pName)
    {
        if(pOffset < mKeys.size())
            mKeys[pOffset]->name = pName;
    }

    void KeyStore::setBackedUp(unsigned int pOffset)
    {
        if(pOffset < mKeys.size())
            mKeys[pOffset]->flags |= PublicKeyData::BACKED_UP;
    }

    void KeyStore::setGap(unsigned int pOffset, unsigned int pGap)
    {
        if(pOffset < mKeys.size())
        {
            if(pGap < Key::DEFAULT_GAP)
                pGap = Key::DEFAULT_GAP;
            mKeys[pOffset]->gap = pGap;
            for(std::vector<Key *>::iterator chainKey = mKeys[pOffset]->chainKeys.begin();
              chainKey != mKeys[pOffset]->chainKeys.end(); ++chainKey)
                (*chainKey)->updateGap(pGap);
        }
    }

    void KeyStore::clear()
    {
        for(std::vector<PublicKeyData *>::iterator key = mKeys.begin(); key != mKeys.end(); ++key)
            delete *key;
        mKeys.clear();

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            delete *key;
        mPrivateKeys.clear();

        mPrivateLoaded = false;
    }

    // If this is the address level then search for public address with matching hash
    Key *KeyStore::findAddress(const NextCash::Hash &pHash)
    {
        Key *result = NULL;
        for(std::vector<PublicKeyData *>::iterator keyData = mKeys.begin(); keyData != mKeys.end();
          ++keyData)
            for(std::vector<Key *>::iterator key = (*keyData)->chainKeys.begin();
              key != (*keyData)->chainKeys.end(); ++key)
            {
                result = (*key)->findAddress(pHash);
                if(result != NULL)
                    return result;
            }

        return NULL;
    }

    Key *KeyStore::findAddress(unsigned int pKeyOffset, const NextCash::Hash &pHash)
    {
        Key *result = NULL;
        PublicKeyData *keyData = mKeys.at(pKeyOffset);
        for(std::vector<Key *>::iterator key = keyData->chainKeys.begin();
          key != keyData->chainKeys.end(); ++key)
        {
            result = (*key)->findAddress(pHash);
            if(result != NULL)
                return result;
        }

        return NULL;
    }

    Key *KeyStore::markUsed(const NextCash::Hash &pHash, bool &pNewAddresses)
    {
        pNewAddresses = false;
        Key *result = NULL;
        for(std::vector<PublicKeyData *>::iterator keyData = mKeys.begin(); keyData != mKeys.end();
          ++keyData)
            for(std::vector<Key *>::iterator key = (*keyData)->chainKeys.begin();
              key != (*keyData)->chainKeys.end(); ++key)
            {
                result = (*key)->markUsed(pHash, (*keyData)->gap, pNewAddresses);
                if(result != NULL)
                    return result;
            }

        return NULL;
    }

    void KeyStore::write(NextCash::OutputStream *pStream) const
    {
        // Version
        pStream->writeUnsignedInt(4);

        // Keys
        pStream->writeUnsignedInt(mKeys.size());
        for(std::vector<PublicKeyData *>::const_iterator keyData = mKeys.begin();
          keyData != mKeys.end(); ++keyData)
            (*keyData)->write(pStream);
    }

    bool KeyStore::read(NextCash::InputStream *pStream)
    {
        clear();

        if(pStream->remaining() < 8)
            return false;

        // Version
        unsigned int version = pStream->readUnsignedInt();
        if(version < 1 || version > 4)
            return false;

        // Keys
        unsigned int count = pStream->readUnsignedInt();
        PublicKeyData *newPublicKey;

        mKeys.reserve(count);
        for(unsigned int i = 0; i < count; ++i)
        {
            newPublicKey = new PublicKeyData();
            if(newPublicKey->read(pStream, version))
                mKeys.push_back(newPublicKey);
            else
            {
                delete newPublicKey;
                return false;
            }
        }

        mPrivateLoaded = count == 0; // If there are no keys there is no private file to load.
        return true;
    }

    bool KeyStore::writePrivate(NextCash::OutputStream *pStream, const uint8_t *pKey,
      unsigned int pKeyLength) const
    {
        if(mPrivateKeys.size() != mKeys.size())
        {
            NextCash::Log::addFormatted(NextCash::Log::WARNING, BITCOIN_KEY_LOG_NAME,
              "Private/public key counts don't match");
            return false;
        }

        // Version
        pStream->writeUnsignedInt(1);

        // Setup encryptor
        NextCash::Encryptor encryptor(pStream, NextCash::Encryption::AES_256,
          NextCash::Encryption::CBC);
        NextCash::Buffer initVector;

        initVector.writeHex(sEncryptKeyInitVector);

        encryptor.setup(pKey, pKeyLength, initVector.begin(), initVector.length());

        // Private Keys
        encryptor.writeUnsignedInt(mPrivateKeys.size());
        for(std::vector<PrivateKeyData *>::const_iterator keyData = mPrivateKeys.begin();
          keyData != mPrivateKeys.end(); ++keyData)
            (*keyData)->write(&encryptor);

        encryptor.finalize();
        return true;
    }

    bool KeyStore::readPrivate(NextCash::InputStream *pStream, const uint8_t *pKey,
      unsigned int pKeyLength)
    {
        if(mPrivateLoaded)
            return true;

        if(pStream->remaining() < 8)
            return false;

        // Version
        unsigned int version = pStream->readUnsignedInt();
        if(version != 1)
            return false;

        // Setup decryptor
        NextCash::Decryptor decryptor(pStream, NextCash::Encryption::AES_256,
          NextCash::Encryption::CBC);
        NextCash::Buffer initVector;

        initVector.writeHex(sEncryptKeyInitVector);

        decryptor.setup(pKey, pKeyLength, initVector.begin(), initVector.length());

        // Read private keys
        unsigned int count = decryptor.readUnsignedInt();

        if(count > 256)
            return false; // Key invalid or file invalid

        PrivateKeyData *newPrivateKey;

        mPrivateKeys.reserve(count);
        for(unsigned int i = 0; i < count; ++i)
        {
            newPrivateKey = new PrivateKeyData();
            if(newPrivateKey->read(&decryptor, version))
                mPrivateKeys.push_back(newPrivateKey);
            else
            {
                delete newPrivateKey;
                return false;
            }
        }

        mPrivateLoaded = true;
        return true;
    }

    void KeyStore::unloadPrivate()
    {
        // Clear private keys
        for(std::vector<PrivateKeyData *>::iterator key=mPrivateKeys.begin();key!=mPrivateKeys.end();++key)
            delete *key;
        mPrivateKeys.clear();

        mPrivateLoaded = mKeys.size() == 0;
    }

    int KeyStore::addKeyPath(Key *pKey, Key::DerivationPathMethod pMethod,
      const std::vector<uint32_t> &pAccountPath, unsigned int pReceivingIndex,
      unsigned int pChangeIndex, int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            if(*(*key)->key == *pKey)
                return 3; // Already exists

        PublicKeyData *newData = new PublicKeyData();
        newData->createdDate = pCreatedDate;

        // Derive account key from master key and derivation path.
        Key *account = pKey->derivePath(pAccountPath);
        if(account == NULL)
            return 6;

        // Receiving
        Key *chain = account->deriveChild(pReceivingIndex);
        std::vector<uint32_t> chainPath = pAccountPath;
        chainPath.emplace_back(pReceivingIndex);

        if(chain->isPrivate())
            newData->chainKeys.push_back(new Key(*chain->publicKey()));
        else
            newData->chainKeys.push_back(new Key(*chain));

        newData->chainKeyPaths.emplace_back(chainPath);

        if(pMethod != Key::INDIVIDUAL && pChangeIndex != pReceivingIndex)
        {
            // Change
            chain = account->deriveChild(pChangeIndex);
            chainPath.back() = pChangeIndex;

            if(chain->isPrivate())
                newData->chainKeys.push_back(new Key(*chain->publicKey()));
            else
                newData->chainKeys.push_back(new Key(*chain));

            newData->chainKeyPaths.emplace_back(chainPath);
        }

        for(std::vector<Key *>::const_iterator key = newData->chainKeys.begin();
          key != newData->chainKeys.end(); ++key)
            (*key)->updateGap(Key::DEFAULT_GAP);

        newData->hasPrivate = true;
        newData->derivationPathMethod = pMethod;
        mKeys.push_back(newData);

        PrivateKeyData *newPrivateData = new PrivateKeyData(pKey);
        mPrivateKeys.push_back(newPrivateData);

        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
          "Added key with path");
        return 0; // Success
    }

    int KeyStore::addKeyMethod(Key *pKey, Key::DerivationPathMethod pMethod, uint32_t pCoinIndex,
      int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            if(*(*key)->key == *pKey)
                return 3; // Already exists

        PublicKeyData *newData = new PublicKeyData();

        newData->createdDate = pCreatedDate;

        // Prime the key for several derivation methods
        switch(pMethod)
        {
        case Key::INDIVIDUAL:
            // Add only address key as chain key
            if(!newData->addChainKeys(pKey, pMethod, pCoinIndex, 0))
            {
                delete newData;
                return 4; // Invalid Derivation Method
            }
            break;

        case Key::SIMPLE:
        case Key::BIP0032:
        case Key::BIP0044:
            // Receiving chain
            if(!newData->addChainKeys(pKey, pMethod, pCoinIndex, 0))
            {
                delete newData;
                return 4; // Invalid Derivation Method
            }

            // Change chain
            if(!newData->addChainKeys(pKey, pMethod, pCoinIndex, 1))
            {
                delete newData;
                return 4; // Invalid Derivation Method
            }
            break;

        case Key::DERIVE_UNKNOWN:
            // Prime the key for several derivation methods

            // SIMPLE
            newData->addChainKeys(pKey, Key::SIMPLE, pCoinIndex, 0); // Receiving chain
            newData->addChainKeys(pKey, Key::SIMPLE, pCoinIndex, 1); // Change chain

            // BIP-0032
            newData->addChainKeys(pKey, Key::BIP0032, pCoinIndex, 0); // Receiving chain
            newData->addChainKeys(pKey, Key::BIP0032, pCoinIndex, 1); // Change chain

            // BIP-0044
            newData->addChainKeys(pKey, Key::BIP0044, pCoinIndex, 0); // Receiving chain
            newData->addChainKeys(pKey, Key::BIP0044, pCoinIndex, 1); // Change chain

            if(newData->chainKeys.size() == 0)
            {
                delete newData;
                return 4; // Invalid Derivation Method
            }
            break;

        case Key::DERIVE_CUSTOM:
            return 4;
        }

        PrivateKeyData *newPrivateData = new PrivateKeyData(pKey);
        mPrivateKeys.push_back(newPrivateData);

        for(std::vector<Key *>::const_iterator key = newData->chainKeys.begin();
          key != newData->chainKeys.end(); ++key)
            (*key)->updateGap(Key::DEFAULT_GAP);

        newData->hasPrivate = pKey->isPrivate();
        newData->derivationPathMethod = pMethod;
        mKeys.push_back(newData);
        return 0; // Success
    }

    int KeyStore::addSeed(const char *pSeed, Key::DerivationPathMethod pMethod,
      const std::vector<uint32_t> &pAccountPath, unsigned int pReceivingIndex,
      unsigned int pChangeIndex, int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        Key *newKey = new Key();
        if(!newKey->loadMnemonicSeed(BitCoin::MAINNET, pSeed))
        {
            delete newKey;
            return 6;
        }

        int result = addKeyPath(newKey, pMethod, pAccountPath, pReceivingIndex, pChangeIndex,
          pCreatedDate);

        if(result != 0)
        {
            delete newKey;
            return result;
        }

        mPrivateKeys.back()->seed = pSeed;
        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
          "Added key from seed");
        return 0; // Success
    }

    int KeyStore::addEncodedKey(const char *pEncodedKey, Key::DerivationPathMethod pMethod,
      const std::vector<uint32_t> &pAccountPath, unsigned int pReceivingIndex,
      unsigned int pChangeIndex, int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        Key *newKey = new Key();
        if(!newKey->decodePrivateKey(pEncodedKey))
        {
            delete newKey;
            return 2;
        }

        int result = addKeyPath(newKey, pMethod, pAccountPath, pReceivingIndex, pChangeIndex,
          pCreatedDate);

        if(result != 0)
        {
            delete newKey;
            return result;
        }

        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
          "Added key from encoded private key");
        return 0; // Success
    }

    int KeyStore::addIndividualKey(Key *pIndividualKey, int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            if(*(*key)->key == *pIndividualKey)
                return 3; // Already exists

        PublicKeyData *newData = new PublicKeyData();
        newData->createdDate = pCreatedDate;

        // Add receiving key
        if(pIndividualKey->isPrivate())
            newData->chainKeys.push_back(new Key(*pIndividualKey->publicKey()));
        else
            newData->chainKeys.push_back(pIndividualKey);
        newData->chainKeyPaths.emplace_back();
        newData->chainKeyPaths.back().emplace_back(pIndividualKey->index());

        newData->hasPrivate = pIndividualKey->isPrivate();
        newData->derivationPathMethod = Key::INDIVIDUAL;
        mKeys.push_back(newData);

        if(pIndividualKey->isPrivate())
            mPrivateKeys.push_back(new PrivateKeyData(pIndividualKey));
        else
            mPrivateKeys.push_back(new PrivateKeyData());

        NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
          "Added key from individual key");
        return 0; // Success
    }

    int KeyStore::addFromChainKeys(Key *pReceivingKey, Key *pChangeKey, int32_t pCreatedDate)
    {
        if(!mPrivateLoaded)
            return 5; // Private keys need to be loaded to add a private key

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            if(*(*key)->key == *pReceivingKey)
                return 3; // Already exists

        for(std::vector<PrivateKeyData *>::iterator key = mPrivateKeys.begin();
          key != mPrivateKeys.end(); ++key)
            if(*(*key)->key == *pChangeKey)
                return 3; // Already exists

        PublicKeyData *newData = new PublicKeyData();
        newData->createdDate = pCreatedDate;

        // Add receiving key
        if(pReceivingKey->isPrivate())
            newData->chainKeys.push_back(new Key(*pReceivingKey->publicKey()));
        else
            newData->chainKeys.push_back(pReceivingKey);
        newData->chainKeyPaths.emplace_back();
        newData->chainKeyPaths.back().emplace_back(pReceivingKey->index());

        // Add change key
        if(pChangeKey->isPrivate())
            newData->chainKeys.push_back(new Key(*pChangeKey->publicKey()));
        else
            newData->chainKeys.push_back(pChangeKey);
        newData->chainKeyPaths.emplace_back();
        newData->chainKeyPaths.back().emplace_back(pChangeKey->index());

        // Update gaps
        for(std::vector<Key *>::const_iterator key = newData->chainKeys.begin();
          key != newData->chainKeys.end(); ++key)
            (*key)->updateGap(Key::DEFAULT_GAP);

        newData->hasPrivate = false; // Need support for multiple private keys to support this.
        newData->derivationPathMethod = Key::DERIVE_UNKNOWN;
        mKeys.push_back(newData);
        mPrivateKeys.push_back(new PrivateKeyData());
        return 0; // Success
    }

    bool KeyStore::loadKeys(NextCash::InputStream *pStream)
    {
        // TODO Add create date to this file format.
        if(!mPrivateLoaded)
            return false; // Private keys need to be loaded to add a key

        NextCash::String line;
        unsigned char nextChar;
        PaymentRequest paymentRequest;
        bool found;
        Key *newKey = new Key();
        PublicKeyData *newData;

        while(pStream->remaining())
        {
            line.clear();

            while(pStream->remaining())
            {
                nextChar = pStream->readByte();
                if(nextChar == '\r' || nextChar == '\n' || nextChar == ' ')
                    break;
                line += nextChar;
            }

            if(line.length())
            {
                if(newKey->decode(line))
                {
                    int result = addKeyMethod(newKey, Key::DERIVE_UNKNOWN, Key::COIN_BITCOIN,
                      getTime());
                    if(result != 0)
                        delete newKey;

                    newKey = new Key();
                }
                else
                {
                    paymentRequest = decodePaymentCode(line);

                    if(paymentRequest.network == MAINNET &&
                      paymentRequest.pubKeyHash.size() == PUB_KEY_HASH_SIZE)
                    {
                        // Check if it is already in this block
                        found = false;
                        for(std::vector<PublicKeyData *>::iterator keyData = mKeys.begin();
                            keyData != mKeys.end() && !found; ++keyData)
                            for(std::vector<Key *>::const_iterator key =
                              (*keyData)->chainKeys.begin(); key != (*keyData)->chainKeys.end();
                              ++key)
                                if((*key)->hash() == paymentRequest.pubKeyHash)
                                {
                                    found = true;
                                    break;
                                }

                        if(!found)
                        {
                            newKey->loadHash(paymentRequest.pubKeyHash);
                            newData = new PublicKeyData();
                            newData->hasPrivate = false;
                            newData->chainKeys.push_back(newKey);
                            mPrivateKeys.push_back(new PrivateKeyData());
                            mKeys.push_back(newData);

                            newKey = new Key();
                        }
                    }
                }
            }
        }

        delete newKey;
        return true;
    }

    bool KeyStore::remove(unsigned int pOffset)
    {
        if(!mPrivateLoaded || pOffset >= mKeys.size())
            return false; // Private keys need to be loaded to remove a key

        std::vector<PublicKeyData *>::iterator publicKey = mKeys.begin() + pOffset;
        std::vector<PrivateKeyData *>::iterator privateKey = mPrivateKeys.begin() + pOffset;

        delete *publicKey;
        delete *privateKey;

        mKeys.erase(publicKey);
        mPrivateKeys.erase(privateKey);

        return true;
    }

    bool Key::test()
    {
        NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
          "------------- Starting Key Tests -------------");

        bool success = true;
        NextCash::Hash hash;
        NextCash::Hash addressHash;

        /******************************************************************************************
         * BIP-0032 Test Seed Check Sums
         *****************************************************************************************/
        unsigned int seedCheckCount = 3;
        const char *seedCheckData[] =
          {
            "abcdef0123456789abcdef0123456789",
            "abcdef0123456789abcdef0123456789abcdef0123456789",
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
          };
        const char *seedCheckText[] =
          {
            "profit hunt scare educate filter shadow quality sadness abuse boss fly bar",
            "profit hunt scare educate filter shadow quality sadness abuse boss fly battle rubber wasp afraid hamster guide entry",
            "profit hunt scare educate filter shadow quality sadness abuse boss fly battle rubber wasp afraid hamster guide essence vibrant task banana pencil owner cloud",
          };

        NextCash::Buffer seedData;
        NextCash::String seed;

        for(unsigned int i = 0; i < seedCheckCount; ++i)
        {
            seedData.clear();
            seedData.writeHex(seedCheckData[i]);
            seed = createMnemonicFromSeed(Mnemonic::English, &seedData);

            if(seed == seedCheckText[i])
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Seed Text %d", i + 1);
            else
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Seed Text %d", i + 1);
            }

            if(Key::validateMnemonicSeed(seed, ""))
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Seed Check Sum %d", i + 1);
            else
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Seed Check Sum %d", i + 1);
            }
        }

        unsigned int seedTextsToValidateLength = 2;
        const char *seedTextsToValidate[] =
          {
            "waste embark lemon divert practice ramp cement orange route reveal live frown",
            "puzzle void garage bicycle reopen kangaroo spread meat evolve lava hungry gas"
          };

        for(unsigned int i = 0; i < seedTextsToValidateLength; ++i)
        {
            if(Key::validateMnemonicSeed(seedTextsToValidate[i], ""))
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Seed Validate %d", i + 1);
            else
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Seed Validate %d", i + 1);
            }
        }

        unsigned int seedTextsToValidateBadLength = 2;
        const char *seedTextsToValidateBad[] =
          {
            "waste embark lemon divert practice ramp cement orange route reveal live gas",
            "puzzle void garage bicycle reopen kangaroo spread meat evolve lava hungry frown"
          };

        for(unsigned int i = 0; i < seedTextsToValidateBadLength; ++i)
        {
            if(!Key::validateMnemonicSeed(seedTextsToValidateBad[i], ""))
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Seed Validate Bad %d", i + 1);
            else
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Seed Validate Bad %d", i + 1);
            }
        }

        /******************************************************************************************
         * BIP-0032 Test 1
         *****************************************************************************************/
        Key keyTree;
        NextCash::Buffer keyTreeSeed;
        NextCash::String correctEncoding, resultEncoding;


        /******************************************************************************************
         * Chain m
         * ext pub: xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8
         * ext prv: xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHi
         *****************************************************************************************/
        if(success)
        {
            keyTreeSeed.clear();
            keyTreeSeed.writeHex("000102030405060708090a0b0c0d0e0f");
            keyTree.loadBinarySeed(MAINNET, &keyTreeSeed);

            resultEncoding = keyTree.encode();
            correctEncoding = "xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHi";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 Master Key Private");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 Master Key Private");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }

            resultEncoding = keyTree.publicKey()->encode();
            correctEncoding = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 Master Key Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 Master Key Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }


        /******************************************************************************************
         * Chain m/0H
         * ext pub: xpub68Gmy5EdvgibQVfPdqkBBCHxA5htiqg55crXYuXoQRKfDBFA1WEjWgP6LHhwBZeNK1VTsfTFUHCdrfp1bgwQ9xv5ski8PX9rL2dZXvgGDnw
         * ext prv: xprv9uHRZZhk6KAJC1avXpDAp4MDc3sQKNxDiPvvkX8Br5ngLNv1TxvUxt4cV1rGL5hj6KCesnDYUhd7oWgT11eZG7XnxHrnYeSvkzY7d2bhkJ7
         *****************************************************************************************/
        Key *m0hKey;
        if(success)
        {
            m0hKey = keyTree.deriveChild(HARDENED + 0);
            if(m0hKey == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0hKey->encode();
                correctEncoding = "xprv9uHRZZhk6KAJC1avXpDAp4MDc3sQKNxDiPvvkX8Br5ngLNv1TxvUxt4cV1rGL5hj6KCesnDYUhd7oWgT11eZG7XnxHrnYeSvkzY7d2bhkJ7";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0hKey->publicKey()->encode();
            correctEncoding = "xpub68Gmy5EdvgibQVfPdqkBBCHxA5htiqg55crXYuXoQRKfDBFA1WEjWgP6LHhwBZeNK1VTsfTFUHCdrfp1bgwQ9xv5ski8PX9rL2dZXvgGDnw";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 m/0H Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }


        /******************************************************************************************
         * Chain m/0H/1
         * ext pub: xpub6ASuArnXKPbfEwhqN6e3mwBcDTgzisQN1wXN9BJcM47sSikHjJf3UFHKkNAWbWMiGj7Wf5uMash7SyYq527Hqck2AxYysAA7xmALppuCkwQ
         * ext prv: xprv9wTYmMFdV23N2TdNG573QoEsfRrWKQgWeibmLntzniatZvR9BmLnvSxqu53Kw1UmYPxLgboyZQaXwTCg8MSY3H2EU4pWcQDnRnrVA1xe8fs
         *****************************************************************************************/
        Key *m0h1Key;
        if(success)
        {
            m0h1Key = m0hKey->deriveChild(1);
            if(m0h1Key == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1 Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0h1Key->encode();
                correctEncoding = "xprv9wTYmMFdV23N2TdNG573QoEsfRrWKQgWeibmLntzniatZvR9BmLnvSxqu53Kw1UmYPxLgboyZQaXwTCg8MSY3H2EU4pWcQDnRnrVA1xe8fs";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H/1 Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H/1 Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0h1Key->publicKey()->encode();
            correctEncoding = "xpub6ASuArnXKPbfEwhqN6e3mwBcDTgzisQN1wXN9BJcM47sSikHjJf3UFHKkNAWbWMiGj7Wf5uMash7SyYq527Hqck2AxYysAA7xmALppuCkwQ";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 m/0H/1 Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1 Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }


        /******************************************************************************************
         * Chain m/0H/1 Public Only Derivation
         * ext pub: xpub6ASuArnXKPbfEwhqN6e3mwBcDTgzisQN1wXN9BJcM47sSikHjJf3UFHKkNAWbWMiGj7Wf5uMash7SyYq527Hqck2AxYysAA7xmALppuCkwQ
         *****************************************************************************************/
        Key *m0h1PublicKey;
        if(success)
        {
            m0h1PublicKey = m0hKey->publicKey()->deriveChild(1);
            if(m0h1PublicKey == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1 Public Only : Derive Failed");
            }
            else
            {
                resultEncoding = m0h1PublicKey->encode();
                correctEncoding = "xpub6ASuArnXKPbfEwhqN6e3mwBcDTgzisQN1wXN9BJcM47sSikHjJf3UFHKkNAWbWMiGj7Wf5uMash7SyYq527Hqck2AxYysAA7xmALppuCkwQ";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H/1 Public Only");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H/1 Public Only");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }


        /******************************************************************************************
         * Chain m/0H/1/2H
         * ext pub: xpub6D4BDPcP2GT577Vvch3R8wDkScZWzQzMMUm3PWbmWvVJrZwQY4VUNgqFJPMM3No2dFDFGTsxxpG5uJh7n7epu4trkrX7x7DogT5Uv6fcLW5
         * ext prv: xprv9z4pot5VBttmtdRTWfWQmoH1taj2axGVzFqSb8C9xaxKymcFzXBDptWmT7FwuEzG3ryjH4ktypQSAewRiNMjANTtpgP4mLTj34bhnZX7UiM
         *****************************************************************************************/
        Key *m0h12hKey;
        if(success)
        {
            m0h12hKey = m0h1Key->deriveChild(HARDENED + 2);
            if(m0h12hKey == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0h12hKey->encode();
                correctEncoding = "xprv9z4pot5VBttmtdRTWfWQmoH1taj2axGVzFqSb8C9xaxKymcFzXBDptWmT7FwuEzG3ryjH4ktypQSAewRiNMjANTtpgP4mLTj34bhnZX7UiM";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H/1/2h Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H/1/2h Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0h12hKey->publicKey()->encode();
            correctEncoding = "xpub6D4BDPcP2GT577Vvch3R8wDkScZWzQzMMUm3PWbmWvVJrZwQY4VUNgqFJPMM3No2dFDFGTsxxpG5uJh7n7epu4trkrX7x7DogT5Uv6fcLW5";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 m/0H/1/2h Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }


        /******************************************************************************************
         * Chain m/0H/1/2H/2
         * ext pub: xpub6FHa3pjLCk84BayeJxFW2SP4XRrFd1JYnxeLeU8EqN3vDfZmbqBqaGJAyiLjTAwm6ZLRQUMv1ZACTj37sR62cfN7fe5JnJ7dh8zL4fiyLHV
         * ext prv: xprvA2JDeKCSNNZky6uBCviVfJSKyQ1mDYahRjijr5idH2WwLsEd4Hsb2Tyh8RfQMuPh7f7RtyzTtdrbdqqsunu5Mm3wDvUAKRHSC34sJ7in334
         *****************************************************************************************/
        Key *m0h12h2Key;
        if(success)
        {
            m0h12h2Key = m0h12hKey->deriveChild(2);
            if(m0h12h2Key == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h/2 Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0h12h2Key->encode();
                correctEncoding = "xprvA2JDeKCSNNZky6uBCviVfJSKyQ1mDYahRjijr5idH2WwLsEd4Hsb2Tyh8RfQMuPh7f7RtyzTtdrbdqqsunu5Mm3wDvUAKRHSC34sJ7in334";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H/1/2h/2 Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H/1/2h/2 Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0h12h2Key->publicKey()->encode();
            correctEncoding = "xpub6FHa3pjLCk84BayeJxFW2SP4XRrFd1JYnxeLeU8EqN3vDfZmbqBqaGJAyiLjTAwm6ZLRQUMv1ZACTj37sR62cfN7fe5JnJ7dh8zL4fiyLHV";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 m/0H/1/2h/2 Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h/2 Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }


        /******************************************************************************************
         * Chain m/0H/1/2H/2/1000000000
         * ext pub: xpub6H1LXWLaKsWFhvm6RVpEL9P4KfRZSW7abD2ttkWP3SSQvnyA8FSVqNTEcYFgJS2UaFcxupHiYkro49S8yGasTvXEYBVPamhGW6cFJodrTHy
         * ext prv: xprvA41z7zogVVwxVSgdKUHDy1SKmdb533PjDz7J6N6mV6uS3ze1ai8FHa8kmHScGpWmj4WggLyQjgPie1rFSruoUihUZREPSL39UNdE3BBDu76
         *****************************************************************************************/
        Key *m0h12h21000000000Key;
        if(success)
        {
            m0h12h21000000000Key = m0h12h2Key->deriveChild(1000000000);
            if(m0h12h21000000000Key == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h/2/1000000000 Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0h12h21000000000Key->encode();
                correctEncoding = "xprvA41z7zogVVwxVSgdKUHDy1SKmdb533PjDz7J6N6mV6uS3ze1ai8FHa8kmHScGpWmj4WggLyQjgPie1rFSruoUihUZREPSL39UNdE3BBDu76";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 1 m/0H/1/2h/2/1000000000 Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 1 m/0H/1/2h/2/1000000000 Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0h12h21000000000Key->publicKey()->encode();
            correctEncoding = "xpub6H1LXWLaKsWFhvm6RVpEL9P4KfRZSW7abD2ttkWP3SSQvnyA8FSVqNTEcYFgJS2UaFcxupHiYkro49S8yGasTvXEYBVPamhGW6cFJodrTHy";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 1 m/0H/1/2h/2/1000000000 Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 1 m/0H/1/2h/2/1000000000 Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }

        /******************************************************************************************
         * BIP-0032 Test 3
         *****************************************************************************************/

        /******************************************************************************************
         * Chain m
         * ext pub: xpub661MyMwAqRbcEZVB4dScxMAdx6d4nFc9nvyvH3v4gJL378CSRZiYmhRoP7mBy6gSPSCYk6SzXPTf3ND1cZAceL7SfJ1Z3GC8vBgp2epUt13
         * ext prv: xprv9s21ZrQH143K25QhxbucbDDuQ4naNntJRi4KUfWT7xo4EKsHt2QJDu7KXp1A3u7Bi1j8ph3EGsZ9Xvz9dGuVrtHHs7pXeTzjuxBrCmmhgC6
         *****************************************************************************************/
        if(success)
        {
            keyTreeSeed.clear();
            keyTreeSeed.writeHex("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be");
            keyTree.loadBinarySeed(MAINNET, &keyTreeSeed);

            resultEncoding = keyTree.encode();
            correctEncoding = "xprv9s21ZrQH143K25QhxbucbDDuQ4naNntJRi4KUfWT7xo4EKsHt2QJDu7KXp1A3u7Bi1j8ph3EGsZ9Xvz9dGuVrtHHs7pXeTzjuxBrCmmhgC6";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 3 Master Key Private");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 3 Master Key Private");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }

            resultEncoding = keyTree.publicKey()->encode();
            correctEncoding = "xpub661MyMwAqRbcEZVB4dScxMAdx6d4nFc9nvyvH3v4gJL378CSRZiYmhRoP7mBy6gSPSCYk6SzXPTf3ND1cZAceL7SfJ1Z3GC8vBgp2epUt13";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 3 Master Key Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 3 Master Key Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }

        /******************************************************************************************
         * Chain m/0H
         * ext pub: xpub68NZiKmJWnxxS6aaHmn81bvJeTESw724CRDs6HbuccFQN9Ku14VQrADWgqbhhTHBaohPX4CjNLf9fq9MYo6oDaPPLPxSb7gwQN3ih19Zm4Y
         * ext prv: xprv9uPDJpEQgRQfDcW7BkF7eTya6RPxXeJCqCJGHuCJ4GiRVLzkTXBAJMu2qaMWPrS7AANYqdq6vcBcBUdJCVVFceUvJFjaPdGZ2y9WACViL4L
         *****************************************************************************************/
        if(success)
        {
            m0hKey = keyTree.deriveChild(HARDENED + 0);
            if(m0hKey == NULL)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 3 m/0H Private : Derive Failed");
            }
            else
            {
                resultEncoding = m0hKey->encode();
                correctEncoding = "xprv9uPDJpEQgRQfDcW7BkF7eTya6RPxXeJCqCJGHuCJ4GiRVLzkTXBAJMu2qaMWPrS7AANYqdq6vcBcBUdJCVVFceUvJFjaPdGZ2y9WACViL4L";
                if(correctEncoding == resultEncoding)
                    NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Test 3 m/0H Private");
                else
                {
                    success = false;
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Test 3 m/0H Private");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctEncoding.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", resultEncoding.text());
                }

            }
        }

        if(success)
        {
            resultEncoding = m0hKey->publicKey()->encode();
            correctEncoding = "xpub68NZiKmJWnxxS6aaHmn81bvJeTESw724CRDs6HbuccFQN9Ku14VQrADWgqbhhTHBaohPX4CjNLf9fq9MYo6oDaPPLPxSb7gwQN3ih19Zm4Y";
            if(correctEncoding == resultEncoding)
                NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed BIP-0032 Test 3 m/0H Public");
            else
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Test 3 m/0H Public");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultEncoding.text());
            }
        }

        /******************************************************************************************
         * BIP-0039 Trezor Test Vectors
         *****************************************************************************************/
        NextCash::Buffer resultSeed, correctSeed, mnemonicStream;
        NextCash::Buffer resultProcessedSeed, correctProcessedSeed;
        NextCash::String resultMnemonic, correctMnemonic;
        unsigned int trezorCount = 24;
        PaymentRequest paymentRequest;

        const char *trezorSeedHex[] =
        {
            "00000000000000000000000000000000",
            "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
            "80808080808080808080808080808080",
            "ffffffffffffffffffffffffffffffff",
            "000000000000000000000000000000000000000000000000",
            "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
            "808080808080808080808080808080808080808080808080",
            "ffffffffffffffffffffffffffffffffffffffffffffffff",
            "0000000000000000000000000000000000000000000000000000000000000000",
            "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
            "8080808080808080808080808080808080808080808080808080808080808080",
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            "9e885d952ad362caeb4efe34a8e91bd2",
            "6610b25967cdcca9d59875f5cb50b0ea75433311869e930b",
            "68a79eaca2324873eacc50cb9c6eca8cc68ea5d936f98787c60c7ebc74e6ce7c",
            "c0ba5a8e914111210f2bd131f3d5e08d",
            "6d9be1ee6ebd27a258115aad99b7317b9c8d28b6d76431c3",
            "9f6a2878b2520799a44ef18bc7df394e7061a224d2c33cd015b157d746869863",
            "23db8160a31d3e0dca3688ed941adbf3",
            "8197a4a47f0425faeaa69deebc05ca29c0a5b5cc76ceacc0",
            "066dca1a2bb7e8a1db2832148ce9933eea0f3ac9548d793112d9a95c9407efad",
            "f30f8c1da665478f49b001d94c5fc452",
            "c10ec20dc3cd9f652c7fac2f1230f7a3c828389a14392f05",
            "f585c11aec520db57dd353c69554b21a89b20fb0650966fa0a9d6f74fd989d8f"
        };

        const char *trezorSeedMnemonic[] =
        {
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
            "legal winner thank year wave sausage worth useful legal winner thank yellow",
            "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong",
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon agent",
            "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful legal will",
            "letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount doctor acoustic avoid letter always",
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo when",
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art",
            "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth title",
            "letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount doctor acoustic bless",
            "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote",
            "ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic",
            "gravity machine north sort system female filter attitude volume fold club stay feature office ecology stable narrow fog",
            "hamster diagram private dutch cause delay private meat slide toddler razor book happy fancy gospel tennis maple dilemma loan word shrug inflict delay length",
            "scheme spot photo card baby mountain device kick cradle pact join borrow",
            "horn tenant knee talent sponsor spell gate clip pulse soap slush warm silver nephew swap uncle crack brave",
            "panda eyebrow bullet gorilla call smoke muffin taste mesh discover soft ostrich alcohol speed nation flash devote level hobby quick inner drive ghost inside",
            "cat swing flag economy stadium alone churn speed unique patch report train",
            "light rule cinnamon wrap drastic word pride squirrel upgrade then income fatal apart sustain crack supply proud access",
            "all hour make first leader extend hole alien behind guard gospel lava path output census museum junior mass reopen famous sing advance salt reform",
            "vessel ladder alter error federal sibling chat ability sun glass valve picture",
            "scissors invite lock maple supreme raw rapid void congress muscle digital elegant little brisk hair mango congress clump",
            "void come effort suffer camp survey warrior heavy shoot primary clutch crush open amazing screen patrol group space point ten exist slush involve unfold"
        };

        const char *trezorProcessedSeed[] =
        {
            "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04",
            "2e8905819b8723fe2c1d161860e5ee1830318dbf49a83bd451cfb8440c28bd6fa457fe1296106559a3c80937a1c1069be3a3a5bd381ee6260e8d9739fce1f607",
            "d71de856f81a8acc65e6fc851a38d4d7ec216fd0796d0a6827a3ad6ed5511a30fa280f12eb2e47ed2ac03b5c462a0358d18d69fe4f985ec81778c1b370b652a8",
            "ac27495480225222079d7be181583751e86f571027b0497b5b5d11218e0a8a13332572917f0f8e5a589620c6f15b11c61dee327651a14c34e18231052e48c069",
            "035895f2f481b1b0f01fcf8c289c794660b289981a78f8106447707fdd9666ca06da5a9a565181599b79f53b844d8a71dd9f439c52a3d7b3e8a79c906ac845fa",
            "f2b94508732bcbacbcc020faefecfc89feafa6649a5491b8c952cede496c214a0c7b3c392d168748f2d4a612bada0753b52a1c7ac53c1e93abd5c6320b9e95dd",
            "107d7c02a5aa6f38c58083ff74f04c607c2d2c0ecc55501dadd72d025b751bc27fe913ffb796f841c49b1d33b610cf0e91d3aa239027f5e99fe4ce9e5088cd65",
            "0cd6e5d827bb62eb8fc1e262254223817fd068a74b5b449cc2f667c3f1f985a76379b43348d952e2265b4cd129090758b3e3c2c49103b5051aac2eaeb890a528",
            "bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd3097170af7a4d73245cafa9c3cca8d561a7c3de6f5d4a10be8ed2a5e608d68f92fcc8",
            "bc09fca1804f7e69da93c2f2028eb238c227f2e9dda30cd63699232578480a4021b146ad717fbb7e451ce9eb835f43620bf5c514db0f8add49f5d121449d3e87",
            "c0c519bd0e91a2ed54357d9d1ebef6f5af218a153624cf4f2da911a0ed8f7a09e2ef61af0aca007096df430022f7a2b6fb91661a9589097069720d015e4e982f",
            "dd48c104698c30cfe2b6142103248622fb7bb0ff692eebb00089b32d22484e1613912f0a5b694407be899ffd31ed3992c456cdf60f5d4564b8ba3f05a69890ad",
            "274ddc525802f7c828d8ef7ddbcdc5304e87ac3535913611fbbfa986d0c9e5476c91689f9c8a54fd55bd38606aa6a8595ad213d4c9c9f9aca3fb217069a41028",
            "628c3827a8823298ee685db84f55caa34b5cc195a778e52d45f59bcf75aba68e4d7590e101dc414bc1bbd5737666fbbef35d1f1903953b66624f910feef245ac",
            "64c87cde7e12ecf6704ab95bb1408bef047c22db4cc7491c4271d170a1b213d20b385bc1588d9c7b38f1b39d415665b8a9030c9ec653d75e65f847d8fc1fc440",
            "ea725895aaae8d4c1cf682c1bfd2d358d52ed9f0f0591131b559e2724bb234fca05aa9c02c57407e04ee9dc3b454aa63fbff483a8b11de949624b9f1831a9612",
            "fd579828af3da1d32544ce4db5c73d53fc8acc4ddb1e3b251a31179cdb71e853c56d2fcb11aed39898ce6c34b10b5382772db8796e52837b54468aeb312cfc3d",
            "72be8e052fc4919d2adf28d5306b5474b0069df35b02303de8c1729c9538dbb6fc2d731d5f832193cd9fb6aeecbc469594a70e3dd50811b5067f3b88b28c3e8d",
            "deb5f45449e615feff5640f2e49f933ff51895de3b4381832b3139941c57b59205a42480c52175b6efcffaa58a2503887c1e8b363a707256bdd2b587b46541f5",
            "4cbdff1ca2db800fd61cae72a57475fdc6bab03e441fd63f96dabd1f183ef5b782925f00105f318309a7e9c3ea6967c7801e46c8a58082674c860a37b93eda02",
            "26e975ec644423f4a4c4f4215ef09b4bd7ef924e85d1d17c4cf3f136c2863cf6df0a475045652c57eb5fb41513ca2a2d67722b77e954b4b3fc11f7590449191d",
            "2aaa9242daafcee6aa9d7269f17d4efe271e1b9a529178d7dc139cd18747090bf9d60295d0ce74309a78852a9caadf0af48aae1c6253839624076224374bc63f",
            "7b4a10be9d98e6cba265566db7f136718e1398c71cb581e1b2f464cac1ceedf4f3e274dc270003c670ad8d02c4558b2f8e39edea2775c9e232c7cb798b069e88",
            "01f5bced59dec48e362f2c45b5de68b9fd6c92c6634f44d6d40aab69056506f0e35524a518034ddc1192e1dacd32c1ed3eaa3c3b131c88ed8e7e54c49a5d0998"
        };

        const char *trezorKeyEncoding[] =
        {
            "xprv9s21ZrQH143K3h3fDYiay8mocZ3afhfULfb5GX8kCBdno77K4HiA15Tg23wpbeF1pLfs1c5SPmYHrEpTuuRhxMwvKDwqdKiGJS9XFKzUsAF",
            "xprv9s21ZrQH143K2gA81bYFHqU68xz1cX2APaSq5tt6MFSLeXnCKV1RVUJt9FWNTbrrryem4ZckN8k4Ls1H6nwdvDTvnV7zEXs2HgPezuVccsq",
            "xprv9s21ZrQH143K2shfP28KM3nr5Ap1SXjz8gc2rAqqMEynmjt6o1qboCDpxckqXavCwdnYds6yBHZGKHv7ef2eTXy461PXUjBFQg6PrwY4Gzq",
            "xprv9s21ZrQH143K2V4oox4M8Zmhi2Fjx5XK4Lf7GKRvPSgydU3mjZuKGCTg7UPiBUD7ydVPvSLtg9hjp7MQTYsW67rZHAXeccqYqrsx8LcXnyd",
            "xprv9s21ZrQH143K3mEDrypcZ2usWqFgzKB6jBBx9B6GfC7fu26X6hPRzVjzkqkPvDqp6g5eypdk6cyhGnBngbjeHTe4LsuLG1cCmKJka5SMkmU",
            "xprv9s21ZrQH143K3Lv9MZLj16np5GzLe7tDKQfVusBni7toqJGcnKRtHSxUwbKUyUWiwpK55g1DUSsw76TF1T93VT4gz4wt5RM23pkaQLnvBh7",
            "xprv9s21ZrQH143K3VPCbxbUtpkh9pRG371UCLDz3BjceqP1jz7XZsQ5EnNkYAEkfeZp62cDNj13ZTEVG1TEro9sZ9grfRmcYWLBhCocViKEJae",
            "xprv9s21ZrQH143K36Ao5jHRVhFGDbLP6FCx8BEEmpru77ef3bmA928BxsqvVM27WnvvyfWywiFN8K6yToqMaGYfzS6Db1EHAXT5TuyCLBXUfdm",
            "xprv9s21ZrQH143K32qBagUJAMU2LsHg3ka7jqMcV98Y7gVeVyNStwYS3U7yVVoDZ4btbRNf4h6ibWpY22iRmXq35qgLs79f312g2kj5539ebPM",
            "xprv9s21ZrQH143K3Y1sd2XVu9wtqxJRvybCfAetjUrMMco6r3v9qZTBeXiBZkS8JxWbcGJZyio8TrZtm6pkbzG8SYt1sxwNLh3Wx7to5pgiVFU",
            "xprv9s21ZrQH143K3CSnQNYC3MqAAqHwxeTLhDbhF43A4ss4ciWNmCY9zQGvAKUSqVUf2vPHBTSE1rB2pg4avopqSiLVzXEU8KziNnVPauTqLRo",
            "xprv9s21ZrQH143K2WFF16X85T2QCpndrGwx6GueB72Zf3AHwHJaknRXNF37ZmDrtHrrLSHvbuRejXcnYxoZKvRquTPyp2JiNG3XcjQyzSEgqCB",
            "xprv9s21ZrQH143K2oZ9stBYpoaZ2ktHj7jLz7iMqpgg1En8kKFTXJHsjxry1JbKH19YrDTicVwKPehFKTbmaxgVEc5TpHdS1aYhB2s9aFJBeJH",
            "xprv9s21ZrQH143K3uT8eQowUjsxrmsA9YUuQQK1RLqFufzybxD6DH6gPY7NjJ5G3EPHjsWDrs9iivSbmvjc9DQJbJGatfa9pv4MZ3wjr8qWPAK",
            "xprv9s21ZrQH143K2XTAhys3pMNcGn261Fi5Ta2Pw8PwaVPhg3D8DWkzWQwjTJfskj8ofb81i9NP2cUNKxwjueJHHMQAnxtivTA75uUFqPFeWzk",
            "xprv9s21ZrQH143K3FperxDp8vFsFycKCRcJGAFmcV7umQmcnMZaLtZRt13QJDsoS5F6oYT6BB4sS6zmTmyQAEkJKxJ7yByDNtRe5asP2jFGhT6",
            "xprv9s21ZrQH143K3R1SfVZZLtVbXEB9ryVxmVtVMsMwmEyEvgXN6Q84LKkLRmf4ST6QrLeBm3jQsb9gx1uo23TS7vo3vAkZGZz71uuLCcywUkt",
            "xprv9s21ZrQH143K2WNnKmssvZYM96VAr47iHUQUTUyUXH3sAGNjhJANddnhw3i3y3pBbRAVk5M5qUGFr4rHbEWwXgX4qrvrceifCYQJbbFDems",
            "xprv9s21ZrQH143K4G28omGMogEoYgDQuigBo8AFHAGDaJdqQ99QKMQ5J6fYTMfANTJy6xBmhvsNZ1CJzRZ64PWbnTFUn6CDV2FxoMDLXdk95DQ",
            "xprv9s21ZrQH143K3wtsvY8L2aZyxkiWULZH4vyQE5XkHTXkmx8gHo6RUEfH3Jyr6NwkJhvano7Xb2o6UqFKWHVo5scE31SGDCAUsgVhiUuUDyh",
            "xprv9s21ZrQH143K3rEfqSM4QZRVmiMuSWY9wugscmaCjYja3SbUD3KPEB1a7QXJoajyR2T1SiXU7rFVRXMV9XdYVSZe7JoUXdP4SRHTxsT1nzm",
            "xprv9s21ZrQH143K2QWV9Wn8Vvs6jbqfF1YbTCdURQW9dLFKDovpKaKrqS3SEWsXCu6ZNky9PSAENg6c9AQYHcg4PjopRGGKmdD313ZHszymnps",
            "xprv9s21ZrQH143K4aERa2bq7559eMCCEs2QmmqVjUuzfy5eAeDX4mqZffkYwpzGQRE2YEEeLVRoH4CSHxianrFaVnMN2RYaPUZJhJx8S5j6puX",
            "xprv9s21ZrQH143K39rnQJknpH1WEPFJrzmAqqasiDcVrNuk926oizzJDDQkdiTvNPr2FYDYzWgiMiC63YmfPAa2oPyNB23r2g7d1yiK6WpqaQS",
        };

        /******************************************************************************************
         * BIP-0039 Trezor Test Vector
         *****************************************************************************************/
        bool trezorPassed = true;
        NextCash::Buffer salt;

        salt.writeString("mnemonicTREZOR");

        for(unsigned int i=0;i<trezorCount;++i)
        {
            correctSeed.clear();
            correctSeed.writeHex(trezorSeedHex[i]);
            correctProcessedSeed.clear();
            correctProcessedSeed.writeHex(trezorProcessedSeed[i]);
            correctMnemonic = trezorSeedMnemonic[i];
            correctEncoding = trezorKeyEncoding[i];

            resultMnemonic = createMnemonicFromSeed(Mnemonic::English, &correctSeed);

            if(resultMnemonic != correctMnemonic)
            {
                trezorPassed = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0039 Trezor Test %d Create Mnemonic", i + 1);
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctMnemonic.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", resultMnemonic.text());
                continue;
            }

            resultProcessedSeed.clear();
            mnemonicStream.clear();
            mnemonicStream.writeString(correctMnemonic);
            processMnemonicSeed(&mnemonicStream, &salt, &resultProcessedSeed);
            if(resultProcessedSeed != correctProcessedSeed)
            {
                trezorPassed = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0039 Trezor Test %d Load Mnemonic : Incorrect Processed Seed",
                  i + 1);
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s",
                  correctProcessedSeed.readHexString(correctProcessedSeed.length()).text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s",
                  resultProcessedSeed.readHexString(resultProcessedSeed.length()).text());
                continue;
            }

            if(!keyTree.loadMnemonicSeed(MAINNET, correctMnemonic, "TREZOR"))
            {
                trezorPassed = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0039 Trezor Test %d Load Mnemonic : Failed to load", i + 1);
                continue;
            }

            if(keyTree.encode() != correctEncoding)
            {
                trezorPassed = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0039 Trezor Test %d Load Mnemonic : Incorrect Key", i + 1);
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.encode().text());
                continue;
            }
        }

        if(trezorPassed)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed BIP-0039 Trezor Test Vector");

        /******************************************************************************************
         * Decode Key Text 1
         *****************************************************************************************/
        if(success)
        {
            correctEncoding = "xprv9s21ZrQH143K3QTDL4LXw2F7HEK3wJUD2nW2nRk4stbPy6cq3jPPqjiChkVvvNKmPGJxWUtg6LnF5kejMRNNU3TGtRBeJgk33yuGBxrMPHi";

            if(!keyTree.decode(correctEncoding))
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 1 : Failed to decode");
            }
            else if(keyTree.encode() != correctEncoding)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 1 : Encode doesn't match");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.encode().text());
            }
            else if(!keyTree.isPrivate())
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 1 : Key not private");
            }
            else if(keyTree.depth() != 0)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 1 : Depth not zero : %d", keyTree.depth());
            }
        }

        if(success)
        {
            correctEncoding = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";
            if(keyTree.publicKey()->encode() != correctEncoding)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 1 : Public encode doesn't match");
            }
        }

        if(success)
            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Decode Key Text 1");

        /******************************************************************************************
         * Decode Key Text 2
         *****************************************************************************************/
        if(success)
        {
            correctEncoding = "xpub661MyMwAqRbcFtXgS5sYJABqqG9YLmC4Q1Rdap9gSE8NqtwybGhePY2gZ29ESFjqJoCu1Rupje8YtGqsefD265TMg7usUDFdp6W1EGMcet8";

            if(!keyTree.decode(correctEncoding))
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 2 : Failed to decode");
            }
            else if(keyTree.encode() != correctEncoding)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 2 : Encode doesn't match");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.encode().text());
            }
            else if(keyTree.isPrivate())
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 2 : Key not public");
            }
            else if(keyTree.depth() != 0)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 2 : Depth not zero : %d", keyTree.depth());
            }
        }

        if(success)
            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Decode Key Text 2");

        /******************************************************************************************
         * Decode Key Text 3
         *****************************************************************************************/
        if(success)
        {
            correctEncoding = "xprvA41z7zogVVwxVSgdKUHDy1SKmdb533PjDz7J6N6mV6uS3ze1ai8FHa8kmHScGpWmj4WggLyQjgPie1rFSruoUihUZREPSL39UNdE3BBDu76";

            if(!keyTree.decode(correctEncoding))
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 3 : Failed to decode");
            }
            else if(keyTree.encode() != correctEncoding)
            {
                success = false;
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 3 : Encode doesn't match");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.encode().text());
            }
            else if(!keyTree.isPrivate())
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 3 : Key not private");
            }
            else if(keyTree.depth() != 5)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 3 : Depth not 5 : %d", keyTree.depth());
            }
        }

        if(success)
        {
            correctEncoding = "xpub6H1LXWLaKsWFhvm6RVpEL9P4KfRZSW7abD2ttkWP3SSQvnyA8FSVqNTEcYFgJS2UaFcxupHiYkro49S8yGasTvXEYBVPamhGW6cFJodrTHy";
            if(keyTree.publicKey()->encode() != correctEncoding)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Decode Key Text 3 : Public encode doesn't match");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.publicKey()->encode().text());
            }
        }

        if(success)
            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Decode Key Text 3");

        /******************************************************************************************
         * Wallet Test vs Electron Cash
         * m/0 For receiving addresses
         * m/1 For change addresses
         *****************************************************************************************/
        Key *chain0, *chain1, *addressKey;
        NextCash::String encodedAddress;
        bool walletSuccess = true;

        if(!keyTree.loadMnemonicSeed(MAINNET,
          "advice cushion arrange charge update kit gloom elbow delay message swap bulk", "",
          "electrum"))
        {
            walletSuccess = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Wallet Test : Failed to load mnemonic");
        }

        if(walletSuccess)
        {
            correctEncoding = "xpub661MyMwAqRbcGujPLVW3q6UQQGetTsUcM7EYwUTDFGif17McpzNmGu5P1kzwxvCNGnjtDPM5MDbRTD8QZQSpktu7f9CcYydG7PNc3tqCKZi";
            if(keyTree.publicKey()->encode() != correctEncoding)
            {
                walletSuccess = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Wallet Test : Public encode doesn't match");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctEncoding.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", keyTree.publicKey()->encode().text());
            }
        }

        chain0 = keyTree.deriveChild(0);
        if(chain0 == NULL)
        {
            walletSuccess = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Wallet Test : Failed create chain 0");
        }

        if(walletSuccess)
        {
            chain1 = keyTree.deriveChild(1);
            if(chain1 == NULL)
            {
                walletSuccess = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Wallet Test : Failed create chain 1");
            }
        }

        if(walletSuccess)
        {
            const char *receivingAddresses[5] =
            {
                "bitcoin:1Jvfk1qMhnZ6i6eWSSkgihwacaTjwABBsr",
                "bitcoin:1JinwuSo1JoUPnxQs3hM4sisyJeNZo3Zvv",
                "bitcoin:1LYZtXwzSHhhFoDyJccjpMWLVZpzgkaZsV",
                "bitcoin:1JK5MMpiTYv8wSZgPZ5oyYZgyVQP6h8prQ",
                "bitcoin:1K2eD9iWqBunMBGWcJBZUSQQmNYHRpk5Ne"
            };

            for(unsigned int i=0;i<5 && walletSuccess;++i)
            {
                addressKey = chain0->deriveChild(i);

                if(addressKey != NULL)
                {
                    encodedAddress = encodePaymentCode(addressKey->hash(),
                      PaymentRequest::Format::LEGACY, MAIN_PUB_KEY_HASH);
                    if(encodedAddress != receivingAddresses[i])
                    {
                        walletSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed to generate receiving address key : %d : Non Matching Address",
                          i);
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Correct : %s", receivingAddresses[i]);
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Result  : %s", encodedAddress.text());
                    }

                    paymentRequest = decodePaymentCode(receivingAddresses[i]);
                    if(paymentRequest.type == AddressType::UNKNOWN)
                    {
                        walletSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed decode address %d", i);
                    }
                    else
                    {
                        if(paymentRequest.network != MAINNET)
                        {
                            walletSuccess = false;
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed decode address network %d", i);
                        }
                        else if(paymentRequest.format != PaymentRequest::LEGACY)
                        {
                            walletSuccess = false;
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed decode address format %d", i);
                        }
                        else if(paymentRequest.pubKeyHash != addressKey->hash())
                        {
                            walletSuccess = false;
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed decode address hash %d", i);
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Correct : %s", addressKey->hash().hex().text());
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Result  : %s", hash.hex().text());
                        }
                    }
                }
                else
                {
                    walletSuccess = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed to generate address key : %d", i);
                }
            }

            const char *changeAddresses[5] =
            {
                "bitcoin:1N89DzxGHj9gfg2uA53QYKuoNX4hhvYwrZ",
                "bitcoin:16xur1hethAuELqR2t5LDAUeUdvUqjgqxW",
                "bitcoin:1BXQQWVzUC6GtutPPvEKnLdXKsFLcGGB9u",
                "bitcoin:1JTxMVtTVJPR1L1WLpH7W3he46Mjatrbk8",
                "bitcoin:1NZbMv8qneXKkexBnjm5BpHMC5teG9BgpS"
            };

            for(unsigned int i=0;i<5 && walletSuccess;++i)
            {
                addressKey = chain1->deriveChild(i);

                if(addressKey != NULL)
                {
                    encodedAddress = encodePaymentCode(addressKey->hash(),
                      PaymentRequest::Format::LEGACY, MAIN_PUB_KEY_HASH);
                    if(encodedAddress != changeAddresses[i])
                    {
                        walletSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed to generate change address key : %d : Non Matching Address", i);
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Correct : %s", receivingAddresses[i]);
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Result  : %s", encodedAddress.text());
                    }
                }
                else
                {
                    walletSuccess = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed to generate address key : %d", i);
                }
            }
        }

        if(walletSuccess)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Wallet Test vs Electron Cash");
        else
            success = false;


        /******************************************************************************************
         * Key Derivation Path Test Vector
         *****************************************************************************************/
        Key *purpose, *coin, *account, *chain, *checkChain;


        /******************************************************************************************
         * SIMPLE external m/0, internal m/1
         *****************************************************************************************/
        // Receiving
        chain = keyTree.deriveChild(0);
        if(chain == NULL)
        {
            success = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed SIMPLE Receiving Chain Key : Failed to derive chain key.");
        }
        else
        {
            checkChain = keyTree.chainKey(0, Key::SIMPLE, 0, 0);
            if(checkChain == NULL)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed SIMPLE Receiving Chain Key : Failed to request chain key.");
            }
            else if(chain->encode() != checkChain->encode())
            {
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed SIMPLE Receiving Chain Key : Non Matching Chain Keys");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", chain->encode().text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", checkChain->encode().text());
            }
            else
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed SIMPLE Receiving Chain Key.");
        }

        // Change
        chain = keyTree.deriveChild(1);
        if(chain == NULL)
        {
            success = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed SIMPLE Change Chain Key : Failed to derive chain key.");
        }
        else
        {
            checkChain = keyTree.chainKey(1, Key::SIMPLE, 0, 0);
            if(checkChain == NULL)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed SIMPLE Change Chain Key : Failed to request chain key.");
            }
            else if(chain->encode() != checkChain->encode())
            {
                NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed SIMPLE Change Chain Key : Non Matching Chain Keys");
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", chain->encode().text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", checkChain->encode().text());
            }
            else
                NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                  "Passed SIMPLE Change Chain Key.");
        }


        /******************************************************************************************
         * BIP-0032 m/0'
         *****************************************************************************************/
        account = keyTree.deriveChild(HARDENED);
        if(account == NULL)
        {
            success = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed BIP-0032 : Failed to derive account key.");
        }
        else
        {
            // Receiving
            chain = account->deriveChild(0);
            if(chain == NULL)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Receiving Chain Key : Failed to derive chain key.");
            }
            else
            {
                checkChain = keyTree.chainKey(0, Key::BIP0032, HARDENED, 0);
                if(checkChain == NULL)
                {
                    success = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Receiving Chain Key : Failed to request chain key.");
                }
                else if(chain->encode() != checkChain->encode())
                {
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Receiving Chain Key : Non Matching Chain Keys");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", chain->encode().text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", checkChain->encode().text());
                }
                else
                    NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Receiving Chain Key.");
            }

            // Change
            chain = account->deriveChild(1);
            if(chain == NULL)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0032 Change Chain Key : Failed to derive chain key.");
            }
            else
            {
                checkChain = keyTree.chainKey(1, Key::BIP0032, HARDENED, 0);
                if(checkChain == NULL)
                {
                    success = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Change Chain Key : Failed to request chain key.");
                }
                else if(chain->encode() != checkChain->encode())
                {
                    NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0032 Change Chain Key : Non Matching Chain Keys");
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", chain->encode().text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", checkChain->encode().text());
                }
                else
                    NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                      "Passed BIP-0032 Change Chain Key.");
            }
        }


        /******************************************************************************************
         * BIP-0044 m/44'/0'/0'
         *****************************************************************************************/
        purpose = keyTree.deriveChild(HARDENED + 44);
        if(purpose == NULL)
        {
            success = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed BIP-0044 : Failed to derive purpose key.");
        }
        else
        {
            coin = purpose->deriveChild(HARDENED);
            if(coin == NULL)
            {
                success = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed BIP-0044 : Failed to derive coin key.");
            }
            else
            {
                account = coin->deriveChild(HARDENED);
                if(account == NULL)
                {
                    success = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed BIP-0044 : Failed to derive account key.");
                }
                else
                {
                    // Receiving
                    chain = account->deriveChild(0);
                    if(chain == NULL)
                    {
                        success = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed BIP-0044 Receiving Chain Key : Failed to derive chain key.");
                    }
                    else
                    {
                        checkChain = keyTree.chainKey(0, Key::BIP0044, HARDENED,
                          Key::COIN_BITCOIN);
                        if(checkChain == NULL)
                        {
                            success = false;
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed BIP-0044 Receiving Chain Key : Failed to request chain key.");
                        }
                        else if(chain->encode() != checkChain->encode())
                        {
                            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed BIP-0044 Receiving Chain Key : Non Matching Chain Keys");
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Correct : %s", chain->encode().text());
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Result  : %s", checkChain->encode().text());
                        }
                        else
                            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                              "Passed BIP-0044 Receiving Chain Key.");
                    }

                    // Change
                    chain = account->deriveChild(1);
                    if(chain == NULL)
                    {
                        success = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed BIP-0044 Change Chain Key : Failed to derive chain key.");
                    }
                    else
                    {
                        checkChain = keyTree.chainKey(1, Key::BIP0044, HARDENED,
                          Key::COIN_BITCOIN);
                        if(checkChain == NULL)
                        {
                            success = false;
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed BIP-0044 Change Chain Key : Failed to request chain key.");
                        }
                        else if(chain->encode() != checkChain->encode())
                        {
                            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Failed BIP-0044 Change Chain Key : Non Matching Chain Keys");
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Correct : %s", chain->encode().text());
                            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                              "Result  : %s", checkChain->encode().text());
                        }
                        else
                            NextCash::Log::addFormatted(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
                              "Passed BIP-0044 Change Chain Key.");
                    }
                }
            }
        }


        /******************************************************************************************
         * Address Gap
         *****************************************************************************************/
        bool addressGapSuccess = true;
        chain = keyTree.chainKey(0, Key::BIP0044, HARDENED, Key::COIN_BITCOIN);
        chain->updateGap(Key::DEFAULT_GAP);

        if(chain->childCount() != Key::DEFAULT_GAP)
        {
            addressGapSuccess = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Address Gap : Update address gap : %d != %d", chain->childCount(),
              Key::DEFAULT_GAP);
        }

        addressKey = chain->getNextUnused();
        if(addressKey == NULL)
        {
            addressGapSuccess = false;
            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Address Gap : Get next unused");
        }

        bool updated = false;
        chain->markUsed(addressKey->hash(), Key::DEFAULT_GAP, updated);

        if(!updated)
        {
            addressGapSuccess = false;
            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Address Gap : Get next unused");
        }

        if(chain->childCount() != 21)
        {
            addressGapSuccess = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Address Gap : Increment address gap : %d != 21", chain->childCount());
        }

        addressKey = chain->findChild(10);
        chain->markUsed(addressKey->hash(), Key::DEFAULT_GAP, updated);

        if(chain->childCount() != 31)
        {
            addressGapSuccess = false;
            NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed Address Gap : Increase address gap : %d != 31", chain->childCount());
        }

        if(addressGapSuccess)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Address Gap");
        else
            success = false;


        /******************************************************************************************
         * KeyStore Read/Write
         *****************************************************************************************/
        NextCash::Buffer keyBuffer;
        KeyStore keyStore;
        bool readWriteSuccess = true;
        Key *newKey = new BitCoin::Key();
        if(newKey->decode("xpub661MyMwAqRbcGujPLVW3q6UQQGetTsUcM7EYwUTDFGif17McpzNmGu5P1kzwxvCNGnjtDPM5MDbRTD8QZQSpktu7f9CcYydG7PNc3tqCKZi"))
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed decode xpub");
        else
        {
            NextCash::Log::add(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
              "Failed to decode xpub");
            success = false;
        }

        keyStore.addFromChainKeys(newKey->deriveChild(0), newKey->deriveChild(1), getTime());

        chain = *keyStore.chainKeys(0)->begin();
        chain->updateGap(5);

        keyStore.write(&keyBuffer);

        keyStore.clear();
        keyStore.read(&keyBuffer);

        chain = *keyStore.chainKeys(0)->begin();

        const char *receivingAddresses[5] =
        {
            "bitcoin:1Jvfk1qMhnZ6i6eWSSkgihwacaTjwABBsr",
            "bitcoin:1JinwuSo1JoUPnxQs3hM4sisyJeNZo3Zvv",
            "bitcoin:1LYZtXwzSHhhFoDyJccjpMWLVZpzgkaZsV",
            "bitcoin:1JK5MMpiTYv8wSZgPZ5oyYZgyVQP6h8prQ",
            "bitcoin:1K2eD9iWqBunMBGWcJBZUSQQmNYHRpk5Ne"
        };

        for(unsigned int i=0;i<5 && readWriteSuccess;++i)
        {
            addressKey = chain->findChild(i);

            if(addressKey != NULL)
            {
                encodedAddress = encodePaymentCode(addressKey->hash(),
                  PaymentRequest::Format::LEGACY);
                if(encodedAddress != receivingAddresses[i])
                {
                    readWriteSuccess = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed to generate receiving address key : %d : Non Matching Address", i);
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", receivingAddresses[i]);
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", encodedAddress.text());
                }

                paymentRequest = decodePaymentCode(receivingAddresses[i]);
                if(paymentRequest.type == AddressType::UNKNOWN)
                {
                    readWriteSuccess = false;
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed decode address %d", i);
                }
                else
                {
                    if(paymentRequest.network != MAINNET)
                    {
                        readWriteSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed decode address network %d", i);
                    }
                    else if(paymentRequest.format != PaymentRequest::LEGACY)
                    {
                        readWriteSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed decode address format %d", i);
                    }
                    else if(paymentRequest.pubKeyHash != addressKey->hash())
                    {
                        readWriteSuccess = false;
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Failed decode address hash %d", i);
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Correct : %s", addressKey->hash().hex().text());
                        NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                          "Result  : %s", hash.hex().text());
                    }
                }
            }
            else
            {
                readWriteSuccess = false;
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed to generate address key : %d", i);
            }
        }

        if(readWriteSuccess)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Key Store Read/Write");
        else
            success = false;


        /******************************************************************************************
         * Encode Cash Address
         *****************************************************************************************/
        unsigned int cashAddressCount = 5;
        NextCash::String cashAddressResult;
        bool cashAddressSuccess = true;
        NextCash::Hash correctAddressHash;
        NextCash::String legacyAddresses[] =
        {
            "1BpEi6DfDAUFd7GtittLSdBeYJvcoaVggu",
            "1KXrWXciRDZUpQwQmuM1DbwsKDLYAYsVLR",
            "16w1D5WRVKJuZUsSRzdLp9w3YGcgoxDXb",
            "3CWFddi6m4ndiGyKqzYvsFYagqDLPVMTzC",
            "3LDsS579y7sruadqu11beEJoTjdFiFCdX4",
            "31nwvkZwyPdgzjBJZXfDmSWsC4ZLKpYyUw",
        };
        NextCash::String correctCashAddresses[] =
        {
            "bitcoincash:qpm2qsznhks23z7629mms6s4cwef74vcwvy22gdx6a",
            "bitcoincash:qr95sy3j9xwd2ap32xkykttr4cvcu7as4y0qverfuy",
            "bitcoincash:qqq3728yw0y47sqn6l2na30mcw6zm78dzqre909m2r",
            "bitcoincash:ppm2qsznhks23z7629mms6s4cwef74vcwvn0h829pq",
            "bitcoincash:pr95sy3j9xwd2ap32xkykttr4cvcu7as4yc93ky28e",
            "bitcoincash:pqq3728yw0y47sqn6l2na30mcw6zm78dzq5ucqzc37",
        };
        NextCash::String upperCashAddresses[] =
        {
            "BITCOINCASH:QPM2QSZNHKS23Z7629MMS6S4CWEF74VCWVY22GDX6A",
            "BITCOINCASH:QR95SY3J9XWD2AP32XKYKTTR4CVCU7AS4Y0QVERFUY",
            "BITCOINCASH:QQQ3728YW0Y47SQN6L2NA30MCW6ZM78DZQRE909M2R",
            "BITCOINCASH:PPM2QSZNHKS23Z7629MMS6S4CWEF74VCWVN0H829PQ",
            "BITCOINCASH:PR95SY3J9XWD2AP32XKYKTTR4CVCU7AS4YC93KY28E",
            "BITCOINCASH:PQQ3728YW0Y47SQN6L2NA30MCW6ZM78DZQ5UCQZC37",
        };

        for(unsigned int i=0;i<cashAddressCount;++i)
        {
            paymentRequest = decodePaymentCode(legacyAddresses[i]);
            if(paymentRequest.format == PaymentRequest::Format::INVALID)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Encode %d : Decode of LEGACY failed", i);
                cashAddressSuccess = false;
                continue;
            }
            else if(paymentRequest.format != PaymentRequest::Format::LEGACY)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Encode %d : Decode format is not LEGACY", i);
                cashAddressSuccess = false;
                continue;
            }

            cashAddressResult = encodePaymentCode(paymentRequest.pubKeyHash,
              PaymentRequest::Format::CASH, paymentRequest.type);

            if(cashAddressResult != correctCashAddresses[i])
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Encode %d : Incorrect encoding", i);
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", cashAddressResult.text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctCashAddresses[i].text());
                cashAddressSuccess = false;
            }
            else
            {
                paymentRequest = decodePaymentCode(correctCashAddresses[i]);
                if(paymentRequest.format == PaymentRequest::Format::INVALID)
                {
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed Cash Address Decode %d : Decode failed", i);
                    cashAddressSuccess = false;
                }
                else if(paymentRequest.format != PaymentRequest::Format::CASH)
                {
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed Cash Address Decode %d : Decode format is not CASH", i);
                    cashAddressSuccess = false;
                }
                else if(addressHash != correctAddressHash)
                {
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed Cash Address Decode %d : Incorrect hash", i);
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Result  : %s", addressHash.hex().text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", correctAddressHash.hex().text());
                    cashAddressSuccess = false;
                }
            }

            // Check uppercase
            paymentRequest = decodePaymentCode(upperCashAddresses[i]);
            if(paymentRequest.format == PaymentRequest::Format::INVALID)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Decode %d : Decode upper failed", i);
                cashAddressSuccess = false;
            }
            else if(paymentRequest.format != PaymentRequest::Format::CASH)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Decode %d : Decode upper format is not CASH", i);
                cashAddressSuccess = false;
            }
            else if(addressHash != correctAddressHash)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Cash Address Decode %d : Incorrect upper hash", i);
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Result  : %s", addressHash.hex().text());
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Correct : %s", correctAddressHash.hex().text());
                cashAddressSuccess = false;
            }
        }

        if(cashAddressSuccess)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Cash Address");
        else
            success = false;

        /******************************************************************************************
         * Dencode Private Key
         *****************************************************************************************/
        unsigned int privateKeyCount = 4;
        NextCash::String encodedPrivateKey, privatePublicKey;
        bool privateKeySuccess = true;
        Key privateKey;
        NextCash::String encodedPrivateKeys[] =
        {
            "KwmjS9XduZYuBLdTAwpYAcrEPDT4ntH7HZsrPkcEw89Uy2ncMSzN",
            "L3mveoTMrLsXCUWBX2amqv5FEepdXqdWJqyNmFdejzvZr35LZS7n",
            "KxU5bMCSrAicKje5GPRXbJGKMcdAY5DusLpAdjsjN3XxUqYaReEn",
            "L5Lo2zy53pC9RhoQuADtqWDLSntutKH35quKyV93UV9X9nL2j938"
        };
        NextCash::String encodedPrivateKeysMatchingPublicKey[] =
        {
            "qzk0f7rgjwl6cm7rkvdzj7s4mvc9fs7yug7tgzz2zz",
            "qqggnyvxe647hwk282gltztrxh3u77jvcclxmq307w",
            "qpsjg5jr6wat0dz9qnatz84j8mull6f5s55a5egnv0",
            "qzavued6cv9r80d8hs5qa549plkac97xduucecj39l"
        };

        for(unsigned int i = 0; i < privateKeyCount; ++i)
        {
            if(!privateKey.decodePrivateKey(encodedPrivateKeys[i]))
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Private Key Decode %d : decode failed", i);
                privateKeySuccess = false;
                break;
            }
            else if(privateKey.version() != MAINNET_PRIVATE)
            {
                NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                  "Failed Private Key Decode %d : key type %02x", i, privateKey.version());
                privateKeySuccess = false;
                break;
            }
            else
            {
                privatePublicKey = encodeCashAddress(privateKey.hash(), MAIN_PUB_KEY_HASH);
                if(privatePublicKey != encodedPrivateKeysMatchingPublicKey[i])
                {
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed Private Key Decode %d : public key not matching", i);
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Generated : %s", privatePublicKey.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", encodedPrivateKeysMatchingPublicKey[i].text());
                    privateKeySuccess = false;
                    break;
                }

                encodedPrivateKey = privateKey.encodePrivateKey();
                if(encodedPrivateKey != encodedPrivateKeys[i])
                {
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Failed Private Key Encode %d", i);
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Generated : %s", encodedPrivateKey.text());
                    NextCash::Log::addFormatted(NextCash::Log::ERROR, BITCOIN_KEY_LOG_NAME,
                      "Correct : %s", encodedPrivateKeys[i].text());
                    privateKeySuccess = false;
                    break;
                }
            }
        }

        if(privateKeySuccess)
            NextCash::Log::add(NextCash::Log::INFO, BITCOIN_KEY_LOG_NAME,
              "Passed Private Key Encoding/Decoding");
        else
            success = false;

        return success;
    }
}
