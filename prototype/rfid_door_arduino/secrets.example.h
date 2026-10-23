#pragma once
// Copy file nay thanh secrets.h (cung thu muc) roi dien thong tin that.
// Xoa dong AP_PASS_PLACEHOLDER o cuoi sau khi dien xong (AP du phong chi bat
// khi mat khau la that).
// secrets.h nam trong .gitignore -> khong bao gio bi commit.
#define WIFI_SSID  "your-wifi-ssid"
#define WIFI_PASS  "your-wifi-pass"
#define WEB_USER   "admin"
#define WEB_PASS   "change-me-now-123"   // >= 12 ky tu
#define AP_PASS    "change-me-123"       // >= 8 ky tu
#define AP_PASS_PLACEHOLDER 1             // XOA dong nay khi da dien thong tin that
