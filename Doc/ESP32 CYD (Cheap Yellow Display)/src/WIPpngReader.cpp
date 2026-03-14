#include <TFT_eSPI.h>
#include <PNGdec.h>
#include <SPIFFS.h>

#define TFT_BACKLIGHT_ON 2  // Adjust to your GPIO pin if needed

TFT_eSPI tft = TFT_eSPI();
PNG png;

// Global File handle (required for PNGdec callbacks)
fs::File pngFile;

// Callback functions for PNGdec
void *fileOpen(const char *filename, int32_t *size) {
    String fullPath = "/" + String(filename);
    pngFile = SPIFFS.open(fullPath, "r");
    if (!pngFile) return nullptr;
    *size = pngFile.size();
    return (void *)&pngFile;
}

void fileClose(void *handle) {
    ((fs::File *)handle)->close();
}

int32_t fileRead(PNGFILE *handle, uint8_t *buffer, int32_t length) {
    return ((fs::File *)handle->fHandle)->read(buffer, length);
}

int32_t fileSeek(PNGFILE *handle, int32_t position) {
    return ((fs::File *)handle->fHandle)->seek(position);
}

// ✅ Final Self-Contained Function
void displayPNGfromSPIFFS(const char *filename, int duration_ms) {
    if (!SPIFFS.begin(true)) {
        Serial.println("Failed to mount SPIFFS!");
        return;
    }

    int16_t rc = png.open(filename, fileOpen, fileClose, fileRead, fileSeek, [](PNGDRAW *pDraw) {
        uint16_t lineBuffer[480];  // Adjust to your screen width if needed
        png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
        tft.pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer);
    });

    if (rc == PNG_SUCCESS) {
        Serial.printf("Displaying PNG: %s\n", filename);
        tft.startWrite();
        png.decode(nullptr, 0);
        tft.endWrite();
    } else {
        Serial.println("PNG decode failed.");
    }

    delay(duration_ms);
}

void setup() {
    Serial.begin(115200);
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    pinMode(TFT_BACKLIGHT_ON, OUTPUT);
    digitalWrite(TFT_BACKLIGHT_ON, HIGH);

    // ✅ Simple call without any "/"
    displayPNGfromSPIFFS("kiki.png", 5000);
}

void loop() {
    // Nothing needed in loop for this example
}
