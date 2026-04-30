#ifndef SPLITITEMS_H
#define SPLITITEMS_H

#include <string>
#include <vector>
#include <map>
#include <pqxx/pqxx>

struct BillItem {
    int itemId;
    std::string name;
    double price;
};

struct Bill {
    int billId;
    std::string storeName;
    std::string date;
    std::vector<BillItem> items;
    double total;
};

class BillSplitter {
public:
    // connectionString: Supabase PostgreSQL connection string
    // userId: the authenticated user's ID (from Supabase Auth)
    BillSplitter(const std::string& connectionString, const std::string& userId);

    // Fetch all bills belonging to the logged-in user
    std::vector<Bill> getUserBills();

    // Load a specific bill by ID (only if it belongs to the logged-in user)
    bool loadBill(int billId);

    // Define the number of people and their names
    void setPeople(int n, const std::vector<std::string>& names);

    // Split a specific item among selected people
    bool splitItem(int itemIndex, const std::vector<std::string>& people);

    // Calculate how much each person owes
    std::map<std::string, double> calculateTotals() const;

    const Bill& getBill() const;
    const std::vector<std::string>& getPeople() const;

private:
    pqxx::connection conn_;
    std::string userId_;
    Bill bill_;
    std::vector<std::string> people_;
    // Maps item index -> list of people sharing that item
    std::map<int, std::vector<std::string>> itemSplits_;
};

#endif
