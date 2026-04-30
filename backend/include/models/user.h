#pragma once
#include <string>

struct User {
    std::string id;           // UUID
    std::string email;
    std::string display_name;
    std::string created_at;
};
