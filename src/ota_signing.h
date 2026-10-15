// ota_signing.h — OTA image signing policy + trusted public key.
//
// OTA images are verified by RSA-2048 / SHA-256 (PKCS#1 v1.5). Sign a build with
// the matching PRIVATE key and upload the detached signature alongside it:
//
//   tools/ota/gen_keys.sh     # one-time: make a keypair, write the key below
//   tools/ota/sign.sh fw.bin  # -> fw.bin.sig (base64)
//
// SECURITY: the key below is this project's real OTA signer (RSA-2048). Its
// private half lives only at tools/ota/ota_private_key.pem (gitignored); keep it
// offline and never commit it. Sign builds with tools/ota/sign.sh. To rotate,
// re-run gen_keys.sh and replace the PEM below.
#pragma once

namespace fogger {

// Require a valid signature for every OTA. Leave true in production. It can be
// turned off for bench work with the build flag -DOTA_REQUIRE_SIGNATURE=0.
#ifndef OTA_REQUIRE_SIGNATURE
#define OTA_REQUIRE_SIGNATURE 1
#endif
#if OTA_REQUIRE_SIGNATURE == 0
#warning "OTA signature verification is DISABLED (bench build) - do not ship this image"
#endif
constexpr bool kOtaRequireSignature = (OTA_REQUIRE_SIGNATURE != 0);

// PEM SubjectPublicKeyInfo of the trusted signer (RSA-2048).
constexpr char kOtaPublicKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzxMvKcntqXDIwNeEYOy4\n"
    "bta0XJEH7Vqy6CrBBzNi8kb1XIOPKPmva1TvTONe6p78pe5pJLD/K49desKo2EoW\n"
    "1RII6eZMZH8tt3oXZ0qnY5TAuNtCU9MufW/XIGDT5FzeUYuE5OzO7XBEYWQK/v5/\n"
    "thCUNoOiPGOdzlPf0jwd6juFdFdLLgVZKByMF0COP6yE04pfmTk3lyXFZvVQHSNX\n"
    "Y2m4JamPAlPbzd5CKf5lcukPSheTJlc+gg1pmB3ZSte9Y/xOTKxSUHlirLB0QbI2\n"
    "rmAiZx4mmOO1BlP1mLzfeZxLpKoyntH4S/UMYzIBRcIBTU94VS8hMPWsPE/3AJUm\n"
    "hwIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

}  // namespace fogger
