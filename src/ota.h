// ota.h — over-the-air firmware update to the inactive OTA slot.
//
// Thin wrapper around the ESP32 Update library, driven chunk-by-chunk by the
// web server's multipart upload handler. On success the caller reboots into the
// new image; on any error the partial write is rolled back.
#pragma once

#include <Arduino.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include <string>

#include "fw_marker.h"

namespace fogger {

class OtaUpdater {
 public:
  // Begin an update. `contentLength` may be 0 if unknown (streamed).
  bool begin(size_t contentLength);
  // Append a chunk of the incoming image. Returns false on a short write.
  bool write(const uint8_t* data, size_t len);
  // Detached base64 signature (RSA-2048 / SHA-256) of the image. Set before
  // end(); required when signing is enforced (see ota_signing.h).
  void setSignatureB64(const std::string& sigB64) { signatureB64_ = sigB64; }
  // Anti-downgrade floor: end() rejects an image whose embedded build number is
  // below this (or missing). Set once at boot from the stored floor.
  void setMinBuild(uint32_t build) { minBuild_ = build; }
  // Build number parsed from the last image (0 if none/not found).
  uint32_t imageBuild() const { return marker_.build(); }
  // Finalize: hash + signature are verified BEFORE the image is made bootable.
  // Returns true only if the whole image was written, signed and verified.
  bool end();
  // Cancel an in-progress update and roll back; `reason` becomes lastError().
  void abort(const char* reason = "aborted");

  bool isActive() const { return active_; }
  // True when every image must carry a valid signature (production builds).
  bool signatureRequired() const;
  // True only after end() verified a complete image; false after any failure.
  bool lastSucceeded() const { return succeeded_; }
  size_t written() const { return written_; }
  const char* lastError() const { return error_.c_str(); }

 private:
  void captureError(const char* context);
  bool verifySignature(const uint8_t* hash32);

  bool active_ = false;
  bool committed_ = false;  // an image has been made bootable; no more until reboot
  bool succeeded_ = false;
  size_t written_ = 0;
  std::string error_;
  std::string signatureB64_;
  mbedtls_sha256_context sha_;
  BuildMarkerScanner marker_;   // recovers the incoming image's build number
  uint32_t minBuild_ = 0;       // reject images older than this
};

}  // namespace fogger
