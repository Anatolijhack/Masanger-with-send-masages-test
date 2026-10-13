#include "ChatService.h"
#include "Session.h"
#include "Logger.h"

using json = nlohmann::json;

std::string ChatService::dump(const json& j)
{
    // replace: битый UTF-8 не бросит исключение, как раньше с 0xEF
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ChatService::can_message(const std::string& a, const std::string& b)
{
    auto staff = [](const std::string& r) { return r == "admin" || r == "employee" || r == "seller"; };
    return staff(a) || staff(b);
}

bool ChatService::origin_allowed(const std::string& origin) const
{
    if (allowed_origins.empty()) return true;   // dev-режим
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();
}

void ChatService::reply_error(const std::shared_ptr<Session>& s, const char* code, const char* text)
{
    s->ws_send(dump({ {"type", "error"}, {"code", code}, {"message", text} }));
}

void ChatService::kick_user(int user_id)
{
    for (auto& s : hub.sessions_of(user_id)) s->ws_close(1008, "session ended");
}

void ChatService::on_binary(const std::shared_ptr<Session>& s)
{
    reply_error(s, "unsupported", "Binary frames are not supported");
}

void ChatService::on_text(const std::shared_ptr<Session>& s, const std::string& raw)
{
    json msg = json::parse(raw, nullptr, false);   // без исключений

    if (msg.is_discarded() || !msg.is_object() || !msg.contains("type") || !msg["type"].is_string())
        return reply_error(s, "bad_request", "Invalid message");

    const std::string type = msg["type"].get<std::string>();

    if (type == "auth") return handle_auth(s, msg);

    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Send auth first");

    if (type == "message") return handle_message(s, msg);

    reply_error(s, "unknown_type", "Unknown message type");
}

void ChatService::handle_auth(const std::shared_ptr<Session>& s, const json& msg)
{
    if (s->chat_identity) return reply_error(s, "already_authenticated", "Already authenticated");

    if (!msg.contains("token") || !msg["token"].is_string())
        return reply_error(s, "bad_request", "token required");

    std::string token = msg["token"].get<std::string>();

    bool queued = workers.try_push_task([this, s, token]()
        {
            auto session = sessions.find(token);          // Ѕƒ Ч в пуле, не в io-потоке

            s->ws_post([this, s, session]()               // обратно в поток сокета
                {
                    if (s->chat_identity) return;

                    if (!session)
                    {
                        reply_error(s, "unauthorized", "Invalid or expired token");
                        s->ws_close(1008, "unauthorized");
                        return;
                    }

                    if (!hub.attach(session->user_id, s))
                    {
                        reply_error(s, "too_many_connections", "Too many open connections");
                        s->ws_close(1008, "too many connections");
                        return;
                    }

                    s->chat_identity = Session::ChatIdentity{ session->user_id, session->username, session->role };
                    s->ws_send(dump({ {"type", "auth_ok"}, {"login", session->username} }));
                });
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

void ChatService::handle_message(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->allow_chat_message())
        return reply_error(s, "rate_limited", "Too many messages");

    if (!msg.contains("to") || !msg["to"].is_string() ||
        !msg.contains("text") || !msg["text"].is_string())
        return reply_error(s, "bad_request", "to and text required");

    std::string to = msg["to"].get<std::string>();
    std::string text = msg["text"].get<std::string>();

    const char* ws = " \t\r\n";
    size_t b = text.find_first_not_of(ws);
    if (b == std::string::npos) return reply_error(s, "empty", "Message is empty");
    text = text.substr(b, text.find_last_not_of(ws) - b + 1);

    if (text.size() > 2000) return reply_error(s, "too_long", "Message is too long (max 2000 bytes)");

    Session::ChatIdentity sender = *s->chat_identity;

    bool queued = workers.try_push_task([this, s, sender, to, text]()
        {
            auto recipient = users.find_by_username(to);

            // Ђнет такогої и Ђнельз€ писатьї отвечаем одинаково Ч логины не перебрать
            if (!recipient || recipient->id == sender.user_id || !can_message(sender.role, recipient->role))
            {
                s->ws_post([this, s]() { reply_error(s, "recipient_not_found", "Recipient not found"); });
                return;
            }

            auto saved = chats.save(sender.user_id, recipient->id, text);

            if (!saved)
            {
                s->ws_post([this, s]() { reply_error(s, "server_error", "Could not save message"); });
                return;
            }

            std::string out = dump({
                {"type", "message"}, {"id", saved->id}, {"from", sender.login},
                {"text", text}, {"ts", saved->created_at}
                });

            for (auto& rs : hub.sessions_of(recipient->id)) rs->ws_send(out);

            s->ws_send(dump({ {"type", "sent"}, {"id", saved->id}, {"to", recipient->username}, {"ts", saved->created_at} }));
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}