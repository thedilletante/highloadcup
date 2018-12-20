#pragma once
#include <string>
#include <iostream>
namespace HLC {
namespace Tools {

    inline std::string extractEmailDomain(const std::string& email) {
        string domain;
        for(int i = email.size() - 1; i >= 0;) {
            const auto& c = email[i--];
            if(c != '@') {
                domain += c;
            } else {
                break;
            }
        }
        uint domainSize = domain.size();
        for(uint j = 0; j < domainSize / 2; ++j) {
            swap(domain[j], domain[domainSize - j - 1]);
        }
        return domain;
    }

}
}