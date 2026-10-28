/**
 * QR code module grid for the app's first-run screen, using the qrcodegen copy that HamClock
 * already ships (ESPHamClock/qrcodegen.c*), so the app needs no QR dependency.
 *
 * Built in the hamclock_engine object library for the engine include path. Pure function; safe
 * on any thread.
 */
#include <cstdint>
#include <string>
#include <vector>

#include "qrcodegen.h"

namespace hcengine {

std::string qrMatrix(const std::string &text) {
  std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), tmp(qrcodegen_BUFFER_LEN_MAX);
  // Medium error correction (boosted if it fits the same size): survives glare on a TV screen.
  if (!qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true))
    return "";
  int n = qrcodegen_getSize(qr.data());
  std::string out;
  out.reserve((size_t)n * (n + 1));
  for (int y = 0; y < n; ++y) {
    if (y) out += '\n';
    for (int x = 0; x < n; ++x) out += qrcodegen_getModule(qr.data(), x, y) ? '1' : '0';
  }
  return out;
}

}  // namespace hcengine
