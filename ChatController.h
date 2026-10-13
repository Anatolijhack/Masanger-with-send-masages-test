#pragma once
#include "ChatRepository.h"
#include "UserRepository.h"
#include "SessionRepository.h"
#include "AuthMiddleware.h"
#include "Router.h"
#include <algorithm>

class ChatController
{
public:
    ChatController(ConnectionPool& pool, SessionRepository& sessions)
        : chats(pool), users(pool), sessions(sessions) {}

    void register_routes(Router& router)
    {
        router.add("GET", "/chat/history", [this](const Request& req) { return history(req); });
    }

private:
    Response history(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto param = [&](const char* k) {
            auto it = req.params.find(k);
            return it == req.params.end() ? std::string() : it->second;
            };

        std::string with = param("with");
        if (with.empty())
            return Response{ R"({"error":"with required"})", "application/json", "400 Bad Request" };

        long long before = 0;
        int limit = 50;
        try
        {
            if (!param("before").empty()) before = std::stoll(param("before"));
            if (!param("limit").empty())  limit = std::clamp(std::stoi(param("limit")), 1, 100);
        }
        catch (...)
        {
            return Response{ R"({"error":"Bad before/limit"})", "application/json", "400 Bad Request" };
        }

        auto other = users.find_by_username(with);
        if (!other) return Response{ R"({"items":[]})", "application/json" };   // как Ђнет сообщенийї

        // выдаютс€ только строки, где ты Ч одна из сторон, чужую переписку не получить
        return Response{ chats.history(auth.user_id, other->id, before, limit), "application/json" };
    }

    ChatRepository chats;
    UserRepository users;
    SessionRepository& sessions;
};