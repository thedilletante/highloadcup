#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <iostream>
#include <time.h>
#include <thread>
#include "profile_tool.h"
#include "db.h"

namespace HLC {
bool TDatabase::LoadFromFile(const std::string& filePath) {
    EXEC_TIME("LoadFromFile");
    using namespace rapidjson;
    FILE* pFile = fopen(filePath.c_str(), "rb");
    char buffer[65536];
    FileReadStream is(pFile, buffer, sizeof(buffer));
    Document document;
    document.ParseStream<0, UTF8<>, FileReadStream>(is);
    const Value& accounts = document["accounts"].GetArray();
    Accounts.reserve(accounts.Size());
    for(uint64_t i = 0; i < accounts.Size(); ++i) {
        ParseJsonAccount(accounts[i]);
    }
    return true;
}

void TDatabase::ParseJsonAccount(const rapidjson::Value& jsonAcc) {
    TEmailType email = jsonAcc["email"].GetString();
    if(EmailKeys.find(email) != EmailKeys.end()) {
        return;
    }

    TPhoneType phone = "";
    if(jsonAcc.HasMember("phone")) {
        phone = jsonAcc["phone"].GetString();
        if (PhoneKeys.find(phone) != PhoneKeys.end()) {
            return;
        }
    }

    auto account = new TAccount {
            static_cast<TIdType>(jsonAcc["id"].GetInt()),
            jsonAcc.HasMember("fname") ? jsonAcc["fname"].GetString() : "",
            jsonAcc.HasMember("sname") ? jsonAcc["sname"].GetString() : "",
            jsonAcc["email"].GetString(),
            {jsonAcc.HasMember("interests") ? jsonAcc["interests"].GetArray().Size() : 0, ""},
            jsonAcc["status"].GetString(),
            jsonAcc.HasMember("premium") ? static_cast<TPremiumTimeType>(jsonAcc["premium"]["start"].GetInt()) : 0,
            jsonAcc.HasMember("premium") ? static_cast<TPremiumTimeType>(jsonAcc["premium"]["finish"].GetInt()) : 0,
            jsonAcc["sex"].GetString()[0] != 'f', // quite sexism
            jsonAcc.HasMember("phone") ? jsonAcc["phone"].GetString() : "",
            {jsonAcc.HasMember("likes") ? jsonAcc["likes"].GetArray().Size() : 0, TLike()},
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
            account->Likes[j].Id =  static_cast<TIdType>(likes[j]["id"].GetUint());
            account->Likes[j].Ts =  static_cast<TTimestampType>(likes[j]["id"].GetUint());
        }
    }

    Accounts.emplace_back(account);
    EmailKeys[email] = account;
    PhoneKeys[phone] = account;
}


void TDatabase::Dump() {
    std::cout << "smth=" << Accounts.size() << std::endl;
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