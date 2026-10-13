#include "ChatRepository.h"
#include "Utf16.h"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

static void print_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR sqlstate[6];
    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native_error;
    SQLSMALLINT message_len;
    SQLSMALLINT i = 1;

    while (SQLGetDiagRecA(handle_type, handle, i, sqlstate, &native_error,
        message, sizeof(message), &message_len) == SQL_SUCCESS)
    {
        std::cerr << "ODBC Error [" << sqlstate << "] " << message << std::endl;
        i++;
    }
}

std::optional<SavedMessage> ChatRepository::save(int sender_id, int recipient_id, const std::string& text)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "INSERT INTO Messages (sender_id, recipient_id, body) "
        "OUTPUT INSERTED.id, CONVERT(NVARCHAR(30), INSERTED.created_at, 126) "
        "VALUES (?, ?, ?)";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN i1 = 0, i2 = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &sender_id, 0, &i1);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &recipient_id, 0, &i2);

    std::wstring wtext = utf8_to_wide(text);
    SQLLEN body_ind = SQL_NTS;
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        wtext.size(), 0, (SQLPOINTER)wtext.c_str(), 0, &body_ind);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return std::nullopt;
    }

    SQLBIGINT id = 0;
    SQLCHAR ts[32] = {};
    SQLLEN id_ind = 0, ts_ind = 0;
    SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);

    std::optional<SavedMessage> out;
    if (SQL_SUCCEEDED(SQLFetch(stmt)))
        out = SavedMessage{ (long long)id, std::string((char*)ts) };

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return out;
}

std::string ChatRepository::history(int me, int other, long long before_id, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT TOP (?) m.id, u.login, m.body, CONVERT(NVARCHAR(30), m.created_at, 126) "
        "FROM Messages m JOIN Users u ON u.id = m.sender_id "
        "WHERE ((m.sender_id = ? AND m.recipient_id = ?) OR (m.sender_id = ? AND m.recipient_id = ?)) "
        "AND (? = 0 OR m.id < ?) "
        "ORDER BY m.id DESC";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN ind[7] = {};
    auto bind_int = [&](int idx, int& v) {
        SQLBindParameter(stmt, idx, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &v, 0, &ind[idx - 1]);
        };
    bind_int(1, limit);
    bind_int(2, me);  bind_int(3, other);
    bind_int(4, other); bind_int(5, me);
    SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[5]);
    SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[6]);

    json items = json::array();

    if (SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        SQLBIGINT id = 0;
        SQLCHAR login[64] = {}, ts[32] = {};
        SQLWCHAR body[2049] = {};
        SQLLEN id_ind = 0, login_ind = 0, body_ind = 0, ts_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
        SQLBindCol(stmt, 2, SQL_C_CHAR, login, sizeof(login), &login_ind);
        SQLBindCol(stmt, 3, SQL_C_WCHAR, body, sizeof(body), &body_ind);
        SQLBindCol(stmt, 4, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);

        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            items.push_back({
                {"id", (long long)id},
                {"from", std::string((char*)login)},
                {"text", wide_to_utf8((const wchar_t*)body, (int)(body_ind / sizeof(wchar_t)))},
                {"ts", std::string((char*)ts)}
                });
        }
    }
    else
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    json out = { {"items", items} };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}