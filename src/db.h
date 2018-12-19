#pragma once
#include <string>

namespace HLC {
class TDatabase {
public:
    bool LoadFromFile(const std::string& filePath);
};
}