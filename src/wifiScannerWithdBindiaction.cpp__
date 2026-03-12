#include <WiFi.h>
String signalQuality(int rssi);
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("🔍 Starting Wi-Fi Scan...");
  
 
}

void loop() {
  // Start scan
  int n = WiFi.scanNetworks();
  Serial.println("📡 Scan complete!");
  
  if (n == 0) {
    Serial.println("❌ No networks found.");
  } else {
    Serial.printf("📶 %d networks found:\n\n", n);
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int32_t rssi = WiFi.RSSI(i);
      String quality = signalQuality(rssi);

      Serial.printf("🔸 SSID: %-25s | RSSI: %4d dBm | Quality: %s\n",
                    ssid.c_str(), rssi, quality.c_str());
    }
  }
  delay(5000);
}

// Returns a quality label based on RSSI value
String signalQuality(int rssi) {
  if (rssi >= -50) return "📶 Excellent";
  else if (rssi >= -60) return "🙂 Good";
  else if (rssi >= -70) return "😐 Fair";
  else if (rssi >= -80) return "😞 Weak";
  else return "🛑 Very Poor";
}
