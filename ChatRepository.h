#pragma once
#include "ConnectionPool.h"
#include <optional>
#include <string>

struct SavedMessage { long long id; std::string created_at; };

class ChatRepository
{
public:
    explicit ChatRepository(ConnectionPool& pool) : pool(pool) {}
    std::optional<SavedMessage> save(int sender_id, int recipient_id, const std::string& text);
    std::string history(int me, int other, long long before_id, int limit);  // {"items":[...]}, новые первыми
private:
    ConnectionPool& pool;
};