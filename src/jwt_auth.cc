/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "jwt_auth.h"

#include <cstring>
#include <ctime>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "third_party/json.hpp"

namespace vsql_rest {

std::string base64url_decode(const std::string& encoded) {
  std::string result;
  // Convert base64url → standard base64.
  std::string b64 = encoded;
  for (char& c : b64) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  // Add padding.
  while (b64.size() % 4 != 0) b64 += '=';

  // Decode using OpenSSL BIO.
  BIO* b64_bio = BIO_new(BIO_f_base64());
  BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
  BIO* mem_bio = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
  BIO_push(b64_bio, mem_bio);

  result.resize(b64.size());
  int decoded_len = BIO_read(b64_bio, result.data(), static_cast<int>(result.size()));
  BIO_free_all(b64_bio);

  if (decoded_len < 0) return {};
  result.resize(decoded_len);
  return result;
}

static JwtClaims parse_claims(const std::string& payload_json) {
  JwtClaims claims;
  try {
    auto j = nlohmann::json::parse(payload_json);
    if (j.contains("sub") && j["sub"].is_string())
      claims.sub = j["sub"].get<std::string>();
    if (j.contains("role") && j["role"].is_string())
      claims.role = j["role"].get<std::string>();
    if (j.contains("exp") && j["exp"].is_number())
      claims.exp = j["exp"].get<long long>();
    if (j.contains("iat") && j["iat"].is_number())
      claims.iat = j["iat"].get<long long>();
    for (auto& [key, val] : j.items()) {
      if (key == "sub" || key == "role" || key == "exp" || key == "iat") continue;
      if (val.is_string())
        claims.extra[key] = val.get<std::string>();
      else if (val.is_number_integer())
        claims.extra[key] = std::to_string(val.get<long long>());
      else if (val.is_boolean())
        claims.extra[key] = val.get<bool>() ? "1" : "0";
    }
  } catch (...) {}
  return claims;
}

static bool check_expiry(const JwtClaims& claims) {
  // Fail closed: a token with no exp claim (claims.exp defaults to 0) would
  // otherwise be accepted forever. Require exp and enforce it.
  if (claims.exp == 0) return false;
  return std::time(nullptr) < claims.exp;
}

// Split a JWT into header.payload.signature parts.
static bool split_jwt(const std::string& token,
                      std::string& header_b64,
                      std::string& payload_b64,
                      std::string& sig_b64) {
  auto p1 = token.find('.');
  if (p1 == std::string::npos) return false;
  auto p2 = token.find('.', p1 + 1);
  if (p2 == std::string::npos) return false;
  header_b64  = token.substr(0, p1);
  payload_b64 = token.substr(p1 + 1, p2 - p1 - 1);
  sig_b64     = token.substr(p2 + 1);
  return true;
}

// Internal: verify with pre-split parts.
static JwtResult verify_hs256_parts(const std::string& hb64,
                                    const std::string& pb64,
                                    const std::string& sb64,
                                    const std::string& secret) {
  JwtResult res;
  std::string signing_input = hb64 + "." + pb64;

  // Compute expected HMAC-SHA256.
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(),
       secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(signing_input.data()),
       signing_input.size(),
       digest, &digest_len);

  // Decode the provided signature.
  std::string provided_sig = base64url_decode(sb64);
  if (provided_sig.size() != digest_len ||
      CRYPTO_memcmp(provided_sig.data(), digest, digest_len) != 0) {
    res.error = "invalid signature";
    return res;
  }

  std::string payload_json = base64url_decode(pb64);
  res.claims = parse_claims(payload_json);

  if (!check_expiry(res.claims)) {
    res.error = "token expired";
    return res;
  }

  res.ok = true;
  return res;
}

// Internal: verify RS256 with pre-split parts.
static JwtResult verify_rs256_parts(const std::string& hb64,
                                    const std::string& pb64,
                                    const std::string& sb64,
                                    const std::string& pubkey_path) {
  JwtResult res;
  // Load RSA public key.
  FILE* f = fopen(pubkey_path.c_str(), "r");
  if (!f) {
    res.error = "cannot open public key: " + pubkey_path;
    return res;
  }
  EVP_PKEY* pkey = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
  fclose(f);
  if (!pkey) {
    res.error = "invalid public key";
    return res;
  }

  std::string signing_input = hb64 + "." + pb64;
  std::string provided_sig = base64url_decode(sb64);

  // Verify with EVP.
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool valid = false;
  if (ctx) {
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(ctx,
            reinterpret_cast<const unsigned char*>(signing_input.data()),
            signing_input.size()) == 1 &&
        EVP_DigestVerifyFinal(ctx,
            reinterpret_cast<const unsigned char*>(provided_sig.data()),
            provided_sig.size()) == 1) {
      valid = true;
    }
    EVP_MD_CTX_free(ctx);
  }
  EVP_PKEY_free(pkey);

  if (!valid) {
    res.error = "invalid signature";
    return res;
  }

  std::string payload_json = base64url_decode(pb64);
  res.claims = parse_claims(payload_json);

  if (!check_expiry(res.claims)) {
    res.error = "token expired";
    return res;
  }

  res.ok = true;
  return res;
}

// Public wrappers for the split-based internal functions.
JwtResult verify_hs256(const std::string& token, const std::string& secret) {
  JwtResult res;
  std::string hb64, pb64, sb64;
  if (!split_jwt(token, hb64, pb64, sb64)) { res.error = "malformed token"; return res; }
  return verify_hs256_parts(hb64, pb64, sb64, secret);
}

JwtResult verify_rs256(const std::string& token, const std::string& pubkey_path) {
  JwtResult res;
  std::string hb64, pb64, sb64;
  if (!split_jwt(token, hb64, pb64, sb64)) { res.error = "malformed token"; return res; }
  return verify_rs256_parts(hb64, pb64, sb64, pubkey_path);
}

JwtResult verify_jwt(const std::string& token,
                     const std::string& hs256_secret,
                     const std::string& rs256_pubkey_path) {
  JwtResult res;

  // Split once; dispatch with pre-split parts to avoid redundant split.
  std::string hb64, pb64, sb64;
  if (!split_jwt(token, hb64, pb64, sb64)) {
    res.error = "malformed token";
    return res;
  }

  std::string header_json = base64url_decode(hb64);
  std::string alg;
  try {
    auto hdr = nlohmann::json::parse(header_json);
    if (hdr.contains("alg") && hdr["alg"].is_string())
      alg = hdr["alg"].get<std::string>();
  } catch (...) {
    res.error = "malformed header";
    return res;
  }

  if (alg == "HS256") {
    if (hs256_secret.empty()) {
      res.error = "HS256 token but vsql_rest_jwt_secret is not set";
      return res;
    }
    return verify_hs256_parts(hb64, pb64, sb64, hs256_secret);
  } else if (alg == "RS256") {
    if (rs256_pubkey_path.empty()) {
      res.error = "RS256 token but vsql_rest_jwt_public_key is not set";
      return res;
    }
    return verify_rs256_parts(hb64, pb64, sb64, rs256_pubkey_path);
  } else {
    res.error = "unsupported algorithm: " + alg;
    return res;
  }
}

}  // namespace vsql_rest
