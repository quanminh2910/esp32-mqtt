#define FIREBASE_MANIFEST_URL "https://YOUR_PROJECT_ID-default-rtdb.firebaseio.com/fota/stm32.json"

// Optional: if your DB rules are not public-read, append ?auth=TOKEN
// Example:
// https://.../fota/stm32.json?auth=YOUR_TOKEN

// Where to save the downloaded STM32 firmware on the ESP32
#define LOCAL_FW_PATH "/stm32_fw.bin"