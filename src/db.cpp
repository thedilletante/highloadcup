#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <iostream>
#include <time.h>
#include <thread>
#include "profile_tool.h"
#include "db.h"
#include "tools.h"

namespace HLC {
bool TDatabase::LoadFromFile(const std::string& filePath) {
    EXEC_TIME("LoadFromFile");
    using namespace rapidjson;
    FILE* pFile = fopen(filePath.c_str(), "rb");
    char buffer[65536];
    FileReadStream is(pFile, buffer, sizeof(buffer));
    Document document;
    document.ParseStream<0, UTF8<>, FileReadStream>(is);
    const auto& accounts = document["accounts"].GetArray();
    ReadThreadCount = 0;
    const auto maxThreadCount = thread::hardware_concurrency();

    const auto accountsSize = accounts.Size();
    for(uint64_t i = 0; i <= accountsSize;) {
        size_t start = i;
        i+=FileReadBlockSize;
        size_t end = i >= accountsSize ? accountsSize : i;

        if(ReadThreadCount > maxThreadCount) {
            unique_lock<mutex> lock(ThreadMtx);
            ThreadWaitCond.wait(lock, [&]() { return ReadThreadCount <= maxThreadCount;});
        }
        ReadThreadCount++;
        thread(&TDatabase::ParseJsonWorker, this, accounts, start, end).detach();
    }
    unique_lock<mutex> lock(ThreadMtx);
    ThreadWaitCond.wait(lock, [&]() {return ReadThreadCount == 0;});
    return true;
}

void TDatabase::ParseJsonWorker(const TAccountsJsonArray& accounts, size_t start, size_t end) {
    for(size_t i = start; i < end; ++i) {
        ParseJsonAccount(accounts[i]);
    }
    ReadThreadCount--;
    ThreadWaitCond.notify_one();
}


void TDatabase::ParseJsonAccount(const TAccountJson& jsonAcc) {
    TEmailType email = jsonAcc["email"].GetString();
    if(EmailKeys.find(email) != EmailKeys.end()) {
        return;
    }
    TPhoneType phone;
    if(jsonAcc.HasMember("phone")) {
        phone = jsonAcc["phone"].GetString();
        if (PhoneKeys.find(phone) != PhoneKeys.end()) {
            return;
        }
    }

    auto id = static_cast<TIdType>(jsonAcc["id"].GetInt());
    if(IdKeys.find(id) != IdKeys.end()) {
        return;;
    }

    auto account = new TAccount {
            id,
            jsonAcc.HasMember("fname") ? jsonAcc["fname"].GetString() : "",
            jsonAcc.HasMember("sname") ? jsonAcc["sname"].GetString() : "",
            jsonAcc["email"].GetString(),
            {jsonAcc.HasMember("interests") ? jsonAcc["interests"].GetArray().Size() : 0, ""},
            jsonAcc["status"].GetString(),
            jsonAcc.HasMember("premium") ? static_cast<TPremiumTimeType>(jsonAcc["premium"]["start"].GetInt()) : 0,
            jsonAcc.HasMember("premium") ? static_cast<TPremiumTimeType>(jsonAcc["premium"]["finish"].GetInt()) : 0,
            jsonAcc["sex"].GetString()[0] != 'f', // quite sexism
            jsonAcc.HasMember("phone") ? jsonAcc["phone"].GetString() : "",
            {},
            static_cast<TIdType>(jsonAcc["birth"].GetInt()),
            jsonAcc.HasMember("city") ? jsonAcc["city"].GetString() : "",
            jsonAcc.HasMember("country") ? jsonAcc["country"].GetString() : "",
            static_cast<TIdType>(jsonAcc["joined"].GetInt()),
    };


    if(jsonAcc.HasMember("interests")) {
        const auto& interests = jsonAcc["interests"].GetArray();
        for(size_t j = 0; j < interests.Size(); ++j) {
            account->Interests[j] = interests[j].GetString();
        }
    }

    if(jsonAcc.HasMember("likes")) {
        const auto& likes = jsonAcc["likes"].GetArray();
        for(size_t j = 0; j < likes.Size(); ++j) {
            account->Likes.insert(static_cast<TIdType>(likes[j]["id"].GetUint()));
        }
    }

    unique_lock<mutex> lock(InsertDataMtx);
    Accounts.push_back(account);
    IdKeys[id] = account;


}

void TDatabase::PrepareKeys() {
    EXEC_TIME("PrepareKeys");
    Accounts.reserve(IdKeys.size());
    EmailKeys.reserve(IdKeys.size());
    EmailDomainKeys.reserve(IdKeys.size());

    for(const auto& accPtr: Accounts) {
        PrepareLikesKeys(accPtr);
        EmailKeys[accPtr->Email] = accPtr;
        EmailDomainKeys[HLC::Tools::extractEmailDomain(accPtr->Email)] = accPtr;
        if(!accPtr->Phone.empty()) {
            PhoneKeys[accPtr->Phone] = accPtr;
        }
    }
    IdKeys.clear(); // that's only use to build LikesKey fast.

}

void TDatabase::PrepareLikesKeys(TAccount* account) {
    for(const auto& e: account->Likes) {
        auto likedAccount = IdKeys.find(e);
        if (likedAccount != IdKeys.end()) {
            LikesKeys[likedAccount->second].push_back(account);
        }
    }
}


void TDatabase::Dump() {
    std::cout << "accountsSize=" << Accounts.size() << std::endl;
//    for(const auto& acc: Accounts) {
//        std::cout
//            << "id: " << std::to_string(acc->Id)
//            << ", email:" << acc->Email
//            << endl;
//    }
}

TDatabase::~TDatabase() {
    for(const auto ptr: Accounts) {
        delete ptr;
    }
}
}
