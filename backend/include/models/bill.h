#pragma once
#include <string>
#include <optional>
#include <vector>

struct BillItem {
    std::string id;
    std::string bill_id;
    std::string name;
    double      price{0.0};
    int         quantity{1};
    std::string currency;
};

struct Bill {
    std::string id;
    std::string split_id;
    std::string store_name;
    std::string date;           // ISO 8601 date string
    std::string currency;
    double      subtotal{0.0};
    double      tax{0.0};
    double      tip{0.0};
    double      fees{0.0};
    std::optional<std::string> payer_member_id;
    std::string created_at;
    std::vector<BillItem> items;  // populated on demand
};

struct ItemAllocation {
    std::string id;
    std::string bill_item_id;
    std::string member_id;
    std::string allocation_mode;  // "ratio" | "amount"
    double      ratio{0.0};
    double      amount{0.0};
};

struct Payment {
    std::string id;
    std::string split_id;
    std::optional<std::string> from_member;
    std::optional<std::string> to_member;
    double      amount{0.0};
    std::string currency;
    std::optional<std::string> method;
    std::optional<std::string> notes;
    std::string paid_at;
};
