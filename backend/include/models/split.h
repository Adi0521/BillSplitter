#pragma once
#include <string>
#include <optional>

struct Split {
    std::string id;
    std::string owner_id;
    std::string name;
    std::string description;
    std::string type;           // "one_time" | "ongoing"
    std::string currency;
    std::string share_token;
    std::string created_at;
    std::optional<std::string> archived_at;
};

struct SplitMember {
    std::string id;
    std::string split_id;
    std::optional<std::string> user_id;
    std::string name;
    std::optional<std::string> email;
    std::string joined_at;
};
