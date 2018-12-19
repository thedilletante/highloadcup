#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <rapidjson/document.h>
#include <thread>


namespace HLC {
using namespace std;

using TIdType = uint32_t;
using TTimestampType = uint32_t;
struct TLike {
    TTimestampType Ts;
    TIdType Id;
};

using TFnameType = string;
using TSnameType = string;
using TEmailType = string;
using TInterestsType = vector<string>;
using TStatusType = string;
using TPremiumTimeType = TTimestampType;
using TSexType = bool;
using TPhoneType = string;
using TLikesType = vector<TLike>;
using TBirthType = TTimestampType;
using TCityType = string;
using TCountryType = string;
using TJoinedType = TTimestampType;


struct TAccount {
    TIdType Id;
    TFnameType Fname; // optional, unicode, .size() < 50
    TSnameType Sname; // optional, unicode, .size() < 50
    TEmailType Email; // unique, unicode, .size() < 100
    TInterestsType Interests; // unicode, row.size() < 100, may be empty
    TStatusType Status; // "свободны", "заняты", "всё сложно"
    TPremiumTimeType PremiumStart; // timestamp, >= 01.01.2018
    TPremiumTimeType PremiumFinish; // timestamp, >= 01.01.2018
    TSexType Sex; // unicode, "m" | "f"
    TPhoneType Phone; // optional, unique, unicode, .size() < 16
    TLikesType Likes; // may be empty, Id always exists in Accounts. ts -- timestamp
    TBirthType Birth; // timestamp 01.01.1950 <= x <= 01.01.2005
    TCityType City; // optional, unicode, .size() < 50
    TCountryType Country; // optional, unicode, .size() < 50
    TJoinedType Joined; // timestamp 01.01.2011 <= x <= 01.01.2018
};

using TEmailKeysType = unordered_map<TEmailType, TAccount*>;
using TPhoneKeysType = unordered_map<TPhoneType , TAccount*>;
using TAccountsType = vector<TAccount*>;
class TDatabase {
public:
    bool LoadFromFile(const std::string& filePath);
    void Dump();
    void ParseJsonAccount(const rapidjson::Value& jsonAcc);
    ~TDatabase();
private:
    TAccountsType Accounts;
    TEmailKeysType EmailKeys;
    TPhoneKeysType PhoneKeys;
    vector<thread> FileReadThreadPool;


};
}