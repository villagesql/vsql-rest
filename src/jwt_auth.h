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

#ifndef VSQL_REST_JWT_AUTH_H
#define VSQL_REST_JWT_AUTH_H

#include <string>
#include <unordered_map>

namespace vsql_rest {

struct JwtClaims {
  std::string sub;
  std::string role;
  long long exp{0};
  long long iat{0};
  std::unordered_map<std::string, std::string> extra;  // additional string claims
};

struct JwtResult {
  bool ok{false};
  std::string error;
  JwtClaims claims;
};

// Decode a base64url-encoded string (no padding required).
std::string base64url_decode(const std::string& encoded);

// Verify a JWT using HMAC-SHA256 (HS256). secret is the raw secret string.
JwtResult verify_hs256(const std::string& token, const std::string& secret);

// Verify a JWT using RSA-SHA256 (RS256). pubkey_path is a PEM file path.
JwtResult verify_rs256(const std::string& token, const std::string& pubkey_path);

// Verify a JWT using whichever algorithm the header declares.
// Uses hs256_secret for HS256 tokens and rs256_pubkey_path for RS256 tokens.
// Returns error if neither is configured for the declared algorithm.
JwtResult verify_jwt(const std::string& token,
                     const std::string& hs256_secret,
                     const std::string& rs256_pubkey_path);

}  // namespace vsql_rest

#endif  // VSQL_REST_JWT_AUTH_H
