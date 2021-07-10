// Copyright (C) 2021 Joel Rosdahl and other contributors
//
// See doc/AUTHORS.adoc for a complete list of contributors.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "RedisStorage.hpp"

#include <Digest.hpp>
#include <Logging.hpp>
#include <fmtmacros.hpp>

#include <hiredis/hiredis.h>

#ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
#endif

namespace storage {
namespace secondary {

const uint64_t DEFAULT_CONNECT_TIMEOUT_MS = 100;
const uint64_t DEFAULT_OPERATION_TIMEOUT_MS = 10000;
const int DEFAULT_PORT = 6379;

static struct timeval
milliseconds_to_timeval(const uint64_t ms)
{
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return tv;
}

static uint64_t
parse_timeout_attribute(const AttributeMap& attributes,
                        const std::string& name,
                        const uint64_t default_value)
{
  const auto it = attributes.find(name);
  if (it == attributes.end()) {
    return default_value;
  } else {
    return Util::parse_unsigned(it->second, 1, 1000 * 3600, "timeout");
  }
}

static nonstd::optional<std::string>
parse_string_attribute(const AttributeMap& attributes, const std::string& name)
{
  const auto it = attributes.find(name);
  if (it == attributes.end()) {
    return nonstd::nullopt;
  }
  return it->second;
}

RedisStorage::RedisStorage(const Url& url, const AttributeMap& attributes)
  : m_url(url),
    m_prefix("ccache"), // TODO: attribute
    m_context(nullptr),
    m_connect_timeout(parse_timeout_attribute(
      attributes, "connect-timeout", DEFAULT_CONNECT_TIMEOUT_MS)),
    m_operation_timeout(parse_timeout_attribute(
      attributes, "operation-timeout", DEFAULT_OPERATION_TIMEOUT_MS)),
    m_username(parse_string_attribute(attributes, "username")),
    m_password(parse_string_attribute(attributes, "password")),
    m_connected(false),
    m_invalid(false)
{
}

RedisStorage::~RedisStorage()
{
  if (m_context) {
    LOG_RAW("Redis disconnect");
    redisFree(m_context);
    m_context = nullptr;
  }
}

int
RedisStorage::connect()
{
  if (m_connected) {
    return REDIS_OK;
  }
  if (m_invalid) {
    return REDIS_ERR;
  }

  if (m_context) {
    if (redisReconnect(m_context) == REDIS_OK) {
      m_connected = true;
      return REDIS_OK;
    }
    LOG("Redis reconnection error: {}", m_context->errstr);
    redisFree(m_context);
    m_context = nullptr;
  }

  ASSERT(m_url.scheme() == "redis");
  const auto& host = m_url.host();
  const auto& port = m_url.port();
  const auto& sock = m_url.path();
  const auto connect_timeout = milliseconds_to_timeval(m_connect_timeout);
  if (!host.empty()) {
    const int p = port.empty() ? DEFAULT_PORT
                               : Util::parse_unsigned(port, 1, 65535, "port");
    LOG("Redis connecting to {}:{} (timeout {} ms)",
        host.c_str(),
        p,
        m_connect_timeout);
    m_context = redisConnectWithTimeout(host.c_str(), p, connect_timeout);
  } else if (!sock.empty()) {
    LOG("Redis connecting to {} (timeout {} ms)",
        sock.c_str(),
        m_connect_timeout);
    m_context = redisConnectUnixWithTimeout(sock.c_str(), connect_timeout);
  } else {
    LOG("Invalid Redis URL: {}", m_url.str());
    m_invalid = true;
    return REDIS_ERR;
  }

  if (!m_context) {
    LOG_RAW("Redis connection error (NULL context)");
    m_invalid = true;
    return REDIS_ERR;
  } else if (m_context->err) {
    LOG("Redis connection error: {}", m_context->errstr);
    m_invalid = true;
    return m_context->err;
  } else {
    if (m_context->connection_type == REDIS_CONN_TCP) {
      LOG("Redis connection to {}:{} OK",
          m_context->tcp.host,
          m_context->tcp.port);
    }
    if (m_context->connection_type == REDIS_CONN_UNIX) {
      LOG("Redis connection to {} OK", m_context->unix_sock.path);
    }
    m_connected = true;

    if (redisSetTimeout(m_context, milliseconds_to_timeval(m_operation_timeout))
        != REDIS_OK) {
      LOG_RAW("Failed to set operation timeout");
    }

    return auth();
  }
}

int
RedisStorage::auth()
{
  if (m_password) {
    bool log_password = false;
    std::string username = m_username ? *m_username : "default";
    std::string password = log_password ? *m_password : "*******";
    LOG("Redis AUTH {} {}", username, password);
    redisReply* reply;
    if (m_username) {
      reply = static_cast<redisReply*>(redisCommand(
        m_context, "AUTH %s %s", m_username->c_str(), m_password->c_str()));
    } else {
      reply = static_cast<redisReply*>(
        redisCommand(m_context, "AUTH %s", m_password->c_str()));
    }
    if (!reply) {
      LOG("Failed to auth {} in redis", username);
      m_invalid = true;
    } else if (reply->type == REDIS_REPLY_ERROR) {
      LOG("Failed to auth {} in redis: {}", username, reply->str);
      m_invalid = true;
    }
    freeReplyObject(reply);
    if (m_invalid) {
      return REDIS_ERR;
    }
  }
  return REDIS_OK;
}

inline bool
is_error(int err)
{
  return err != REDIS_OK;
}

inline bool
is_timeout(int err)
{
#ifdef REDIS_ERR_TIMEOUT
  // Only returned for hiredis version 1.0.0 and above
  return err == REDIS_ERR_TIMEOUT;
#else
  (void)err;
  return false;
#endif
}

nonstd::expected<nonstd::optional<std::string>, SecondaryStorage::Error>
RedisStorage::get(const Digest& key)
{
  int err = connect();
  if (is_timeout(err)) {
    return nonstd::make_unexpected(Error::timeout);
  } else if (is_error(err)) {
    return nonstd::make_unexpected(Error::error);
  }
  const std::string key_string = get_key_string(key);
  LOG("Redis GET {}", key_string);
  redisReply* reply = static_cast<redisReply*>(
    redisCommand(m_context, "GET %s", key_string.c_str()));
  bool found = false;
  bool missing = false;
  std::string value;
  if (!reply) {
    LOG("Failed to get {} from redis", key_string);
  } else if (reply->type == REDIS_REPLY_ERROR) {
    LOG("Failed to get {} from redis: {}", key_string, reply->str);
  } else if (reply->type == REDIS_REPLY_STRING) {
    value = std::string(reply->str, reply->len);
    found = true;
  } else if (reply->type == REDIS_REPLY_NIL) {
    missing = true;
  }
  freeReplyObject(reply);
  if (found) {
    return value;
  } else if (missing) {
    return nonstd::nullopt;
  } else {
    return nonstd::make_unexpected(Error::error);
  }
}

nonstd::expected<bool, SecondaryStorage::Error>
RedisStorage::put(const Digest& key,
                  const std::string& value,
                  bool only_if_missing)
{
  int err = connect();
  if (is_timeout(err)) {
    return nonstd::make_unexpected(Error::timeout);
  } else if (is_error(err)) {
    return nonstd::make_unexpected(Error::error);
  }
  const std::string key_string = get_key_string(key);
  if (only_if_missing) {
    LOG("Redis EXISTS {}", key_string);
    redisReply* reply = static_cast<redisReply*>(
      redisCommand(m_context, "EXISTS %s", key_string.c_str()));
    int count = 0;
    if (!reply) {
      LOG("Failed to check {} in redis", key_string);
    } else if (reply->type == REDIS_REPLY_ERROR) {
      LOG("Failed to check {} in redis: {}", key_string, reply->str);
    } else if (reply->type == REDIS_REPLY_INTEGER) {
      count = reply->integer;
    }
    freeReplyObject(reply);
    if (count > 0) {
      return false;
    }
  }
  LOG("Redis SET {}", key_string);
  redisReply* reply = static_cast<redisReply*>(redisCommand(
    m_context, "SET %s %b", key_string.c_str(), value.data(), value.size()));
  bool stored = false;
  if (!reply) {
    LOG("Failed to set {} to redis", key_string);
  } else if (reply->type == REDIS_REPLY_ERROR) {
    LOG("Failed to set {} to redis: {}", key_string, reply->str);
  } else if (reply->type == REDIS_REPLY_STATUS) {
    stored = true;
  } else {
    LOG("Failed to set {} to redis: {}", key_string, reply->type);
  }
  freeReplyObject(reply);
  if (stored) {
    return true;
  } else {
    return nonstd::make_unexpected(Error::error);
  }
}

nonstd::expected<bool, SecondaryStorage::Error>
RedisStorage::remove(const Digest& key)
{
  int err = connect();
  if (is_timeout(err)) {
    return nonstd::make_unexpected(Error::timeout);
  } else if (is_error(err)) {
    return nonstd::make_unexpected(Error::error);
  }
  const std::string key_string = get_key_string(key);
  LOG("Redis DEL {}", key_string);
  redisReply* reply = static_cast<redisReply*>(
    redisCommand(m_context, "DEL %s", key_string.c_str()));
  bool removed = false;
  bool missing = false;
  if (!reply) {
    LOG("Failed to del {} in redis", key_string);
  } else if (reply->type == REDIS_REPLY_ERROR) {
    LOG("Failed to del {} in redis: {}", key_string, reply->str);
  } else if (reply->type == REDIS_REPLY_INTEGER) {
    if (reply->integer > 0) {
      removed = true;
    } else {
      missing = true;
    }
  }
  freeReplyObject(reply);
  if (removed) {
    return true;
  } else if (missing) {
    return false;
  } else {
    return nonstd::make_unexpected(Error::error);
  }
}

std::string
RedisStorage::get_key_string(const Digest& digest) const
{
  return FMT("{}:{}", m_prefix, digest.to_string());
}

} // namespace secondary
} // namespace storage