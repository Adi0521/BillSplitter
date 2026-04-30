#include "splititems.h"
#include <iostream>
#include <algorithm>
#include <cmath>

BillSplitter::BillSplitter(const std::string& connectionString, const std::string& userId)
    : conn_(connectionString), userId_(userId) {
    bill_ = {};
}

std::vector<Bill> BillSplitter::getUserBills() {
    std::vector<Bill> bills;

    pqxx::work txn(conn_);
    pqxx::result result = txn.exec_params(
        "SELECT b.bill_id, b.store_name, b.date, "
        "COALESCE(SUM(bi.price), 0) AS total "
        "FROM bills b "
        "LEFT JOIN bill_items bi ON b.bill_id = bi.bill_id "
        "WHERE b.user_id = $1 "
        "GROUP BY b.bill_id, b.store_name, b.date "
        "ORDER BY b.date DESC",
        userId_);
    txn.commit();

    for (const auto& row : result) {
        Bill bill;
        bill.billId = row["bill_id"].as<int>();
        bill.storeName = row["store_name"].as<std::string>("");
        bill.date = row["date"].as<std::string>("");
        bill.total = row["total"].as<double>(0.0);
        bills.push_back(bill);
    }

    return bills;
}

bool BillSplitter::loadBill(int billId) {
    bill_ = {};
    bill_.billId = billId;
    bill_.total = 0.0;
    itemSplits_.clear();

    // Query the bill header, scoped to the logged-in user
    pqxx::work txn(conn_);
    pqxx::result billResult = txn.exec_params(
        "SELECT store_name, date FROM bills "
        "WHERE bill_id = $1 AND user_id = $2",
        billId, userId_);

    if (billResult.empty()) {
        std::cerr << "Bill with ID " << billId
                  << " not found for this user." << std::endl;
        txn.commit();
        return false;
    }

    bill_.storeName = billResult[0]["store_name"].as<std::string>("");
    bill_.date = billResult[0]["date"].as<std::string>("");

    // Query the bill items
    pqxx::result itemsResult = txn.exec_params(
        "SELECT item_id, item_name, price FROM bill_items "
        "WHERE bill_id = $1 "
        "ORDER BY item_id",
        billId);
    txn.commit();

    for (const auto& row : itemsResult) {
        BillItem item;
        item.itemId = row["item_id"].as<int>();
        item.name = row["item_name"].as<std::string>("");
        item.price = row["price"].as<double>(0.0);
        bill_.total += item.price;
        bill_.items.push_back(item);
    }

    return true;
}

void BillSplitter::setPeople(int n, const std::vector<std::string>& names) {
    if (n <= 0) {
        std::cerr << "Number of people must be positive." << std::endl;
        return;
    }
    if (static_cast<int>(names.size()) != n) {
        std::cerr << "Number of names (" << names.size()
                  << ") does not match n (" << n << ")." << std::endl;
        return;
    }
    people_ = names;
}

bool BillSplitter::splitItem(int itemIndex, const std::vector<std::string>& people) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(bill_.items.size())) {
        std::cerr << "Item index " << itemIndex << " is out of range." << std::endl;
        return false;
    }
    if (people.empty()) {
        std::cerr << "Must specify at least one person to split with." << std::endl;
        return false;
    }

    // Verify all people are in the known people list
    for (const auto& person : people) {
        if (std::find(people_.begin(), people_.end(), person) == people_.end()) {
            std::cerr << "Person \"" << person << "\" is not in the people list." << std::endl;
            return false;
        }
    }

    itemSplits_[itemIndex] = people;
    return true;
}

std::map<std::string, double> BillSplitter::calculateTotals() const {
    std::map<std::string, double> totals;

    // Initialize all people with 0
    for (const auto& person : people_) {
        totals[person] = 0.0;
    }

    for (const auto& entry : itemSplits_) {
        int itemIndex = entry.first;
        const std::vector<std::string>& people = entry.second;

        double itemPrice = bill_.items[itemIndex].price;
        double share = itemPrice / people.size();
        // Round to 2 decimal places
        share = std::round(share * 100.0) / 100.0;

        for (const auto& person : people) {
            totals[person] += share;
        }
    }

    // Round final totals to 2 decimal places
    for (auto& entry : totals) {
        entry.second = std::round(entry.second * 100.0) / 100.0;
    }

    return totals;
}

const Bill& BillSplitter::getBill() const {
    return bill_;
}

const std::vector<std::string>& BillSplitter::getPeople() const {
    return people_;
}
