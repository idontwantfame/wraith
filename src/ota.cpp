// ota.cpp — OTA update with RSA/SHA-256 signature verification. See ota.h.
#include "ota.h"

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>

#include "ota_signing.h"

namespace fogger {
namespace {
constexpr size_t kHashLen = 32;        // SHA-256
constexpr size_t kMaxSigBytes = 512;   // RSA-4096 ceiling; RSA-2048 uses 256
constexpr size_t kRsa2048SigBytes = 256;  // our signer key is RSA-2048
}  // namespace

bool OtaUpdater::signatureRequired() const { return kOtaRequireSignature; }

void OtaUpdater::captureError(const char* context) {
  error_ = std::string(context) + ": " + Update.errorString();
  log_e("ota: %s", error_.c_str());
}

bool OtaUpdater::begin(size_t contentLength) {
  // Once an image has been committed to the boot slot, refuse any further update
  // until reboot. Otherwise a second (unsigned) part of the same or a concurrent
  // upload could overwrite the just-committed slot while otadata still points at
  // it, and an aborted write does not roll otadata back.
  if (committed_) {
    error_ = "an update is already staged; reboot before flashing again";
    log_w("ota: %s", error_.c_str());
    return false;
  }
  if (active_) return false;  // one update at a time
  error_.clear();
  signatureB64_.clear();
  written_ = 0;
  succeeded_ = false;
  const size_t size = contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(size, U_FLASH)) {
    captureError("begin");
    return false;
  }
  mbedtls_sha256_init(&sha_);
  if (mbedtls_sha256_starts(&sha_, /*is224=*/0) != 0) {
    error_ = "begin: sha256 init failed";
    log_e("ota: %s", error_.c_str());
    Update.abort();
    mbedtls_sha256_free(&sha_);
    return false;
  }
  marker_.reset();  // start scanning the new image for its build number
  active_ = true;
  log_i("ota: started (len=%u, signed=%d)", (unsigned)contentLength,
        (int)kOtaRequireSignature);
  return true;
}

bool OtaUpdater::write(const uint8_t* data, size_t len) {
  if (!active_) return false;
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    captureError("write");
    Update.abort();
    mbedtls_sha256_free(&sha_);
    active_ = false;
    return false;
  }
  if (mbedtls_sha256_update(&sha_, data, len) != 0) {
    error_ = "write: sha256 update failed";
    log_e("ota: %s", error_.c_str());
    Update.abort();
    mbedtls_sha256_free(&sha_);
    active_ = false;
    return false;
  }
  marker_.feed(data, len);  // look for the embedded build marker
  written_ += len;
  return true;
}

bool OtaUpdater::verifySignature(const uint8_t* hash32) {
  unsigned char sig[kMaxSigBytes];
  size_t sigLen = 0;
  int rc = mbedtls_base64_decode(sig, sizeof(sig), &sigLen,
                                 reinterpret_cast<const unsigned char*>(signatureB64_.data()),
                                 signatureB64_.size());
  if (rc != 0) {
    error_ = "signature: base64 decode failed";
    return false;
  }
  // Our signer is RSA-2048, so a valid signature is exactly 256 bytes. Reject
  // anything else up front (defence in depth; mbedtls also checks internally).
  if (sigLen != kRsa2048SigBytes) {
    error_ = "signature: wrong length";
    log_e("ota: %s (%u bytes)", error_.c_str(), (unsigned)sigLen);
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  rc = mbedtls_pk_parse_public_key(
      &pk, reinterpret_cast<const unsigned char*>(kOtaPublicKeyPem),
      sizeof(kOtaPublicKeyPem));
  if (rc != 0) {
    mbedtls_pk_free(&pk);
    error_ = "signature: public key invalid (set your key in ota_signing.h)";
    return false;
  }
  rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash32, kHashLen, sig, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    error_ = "signature: verification failed";
    log_e("ota: %s (rc=-0x%04x)", error_.c_str(), -rc);
    return false;
  }
  return true;
}

bool OtaUpdater::end() {
  if (!active_) return false;
  active_ = false;

  unsigned char hash[kHashLen];
  const int finishRc = mbedtls_sha256_finish(&sha_, hash);
  mbedtls_sha256_free(&sha_);
  if (finishRc != 0) {
    error_ = "end: sha256 finish failed";
    log_e("ota: %s", error_.c_str());
    Update.abort();
    return false;
  }

  // Verify the signature BEFORE committing the image to the boot slot.
  const bool needSig = kOtaRequireSignature || !signatureB64_.empty();
  if (needSig) {
    if (signatureB64_.empty()) {
      error_ = "signature required but none supplied";
      log_e("ota: %s", error_.c_str());
      Update.abort();
      return false;
    }
    if (!verifySignature(hash)) {
      Update.abort();
      return false;
    }
  }

  // Anti-downgrade: a signed-but-older image must still be refused. The image
  // has to carry a build marker, and it must be >= the stored floor.
  if (!marker_.found()) {
    error_ = "no build marker in image (refusing)";
    log_e("ota: %s", error_.c_str());
    Update.abort();
    return false;
  }
  // Exactly one marker: with a stray earlier "WRAITHFW" the build read would be
  // ambiguous (the first one could spoof a higher build past the floor).
  if (marker_.count() != 1) {
    error_ = "image carries more than one build marker (refusing)";
    log_e("ota: %s (%u markers)", error_.c_str(), (unsigned)marker_.count());
    Update.abort();
    return false;
  }
  if (marker_.build() < minBuild_) {
    error_ = "image is older than the installed build (downgrade blocked)";
    log_e("ota: %s (image=%u, floor=%u)", error_.c_str(),
          (unsigned)marker_.build(), (unsigned)minBuild_);
    Update.abort();
    return false;
  }

  if (!Update.end(/*evenIfRemaining=*/true)) {
    captureError("end");
    return false;
  }
  // Update.end() has switched the boot partition: latch out further OTA now,
  // before any later check can return without setting it.
  committed_ = true;
  if (!Update.isFinished()) {
    error_ = "end: image incomplete";
    log_e("ota: %s", error_.c_str());
    return false;
  }
  succeeded_ = true;
  log_i("ota: complete + verified, %u bytes written", (unsigned)written_);
  return true;
}

void OtaUpdater::abort(const char* reason) {
  if (!active_) return;
  Update.abort();
  mbedtls_sha256_free(&sha_);
  active_ = false;
  error_ = reason;
}

}  // namespace fogger
