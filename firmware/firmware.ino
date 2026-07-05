#include "esp_camera.h"
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h" 

#define FLASH_LED_PIN 4

uint32_t photo_counter = 0; // Automatically set at boot
bool sd_card_ready = false;
enum DitherAlgo { FLOYD_STEINBERG, STUCKI, JARVIS_JUDICE_NINKE };

struct CameraModel {
    bool isStreamToSerialEnabled;
    bool isDitheringEnabled;
    bool isInvertEnabled;
    bool isFlashEnabled;
    DitherAlgo algo;
} camera_model;

// --- Dynamic File Scanner ---
void adjust_photo_counter_to_disk() {
    if (!sd_card_ready) return;

    uint32_t highest_index = 0;
    File root = SD_MMC.open("/");
    if (!root) return;

    File file = root.openNextFile();
    while (file) {
        String name = String(file.name());
        if (name.startsWith("photo_") && name.endsWith(".ppm")) {
            int underscore_idx = name.indexOf('_');
            int dot_idx = name.indexOf('.');
            if (underscore_idx != -1 && dot_idx != -1) {
                String index_str = name.substring(underscore_idx + 1, dot_idx);
                uint32_t current_idx = index_str.toInt();
                if (current_idx > highest_index) {
                    highest_index = current_idx;
                }
            }
        }
        file = root.openNextFile();
    }
    
    // Fallback
    photo_counter = (highest_index > 0 || SD_MMC.open("/photo_0.ppm")) ? highest_index + 1 : 0;
}

// --- Native SD Card Initialization ---
void initialize_sd_card() {
    if (!SD_MMC.begin("/sdcard", true)) { // 1-bit mode protects UART line pins 12 & 13
        sd_card_ready = false;
        return;
    }
    
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        sd_card_ready = false;
        return;
    }
    
    sd_card_ready = true; 
    adjust_photo_counter_to_disk(); 
}

// --- Camera Configurations and Functions ---
void set_camera_config_defaults() {}
void set_camera_model_defaults() {
    camera_model.isStreamToSerialEnabled = false;
    camera_model.isDitheringEnabled = true;
    camera_model.isInvertEnabled = false;
    camera_model.isFlashEnabled = false;
    camera_model.algo = FLOYD_STEINBERG;
}
void set_camera_defaults() {
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
    }
}
void lower_brightness() { sensor_t *s = esp_camera_sensor_get(); if(s) s->set_brightness(s, s->status.brightness - 1); }
void add_brightness()   { sensor_t *s = esp_camera_sensor_get(); if(s) s->set_brightness(s, s->status.brightness + 1); }
void lower_contrast()   { sensor_t *s = esp_camera_sensor_get(); if(s) s->set_contrast(s, s->status.contrast - 1); }
void add_contrast()     { sensor_t *s = esp_camera_sensor_get(); if(s) s->set_contrast(s, s->status.contrast + 1); }
void set_dithering(bool en) { camera_model.isDitheringEnabled = en; }
void set_inverted(bool en)  { camera_model.isInvertEnabled = en; }
void set_dithering_algorithm(DitherAlgo a) { camera_model.algo = a; }

void turn_flash_on() { 
    camera_model.isFlashEnabled = true; 
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, HIGH); 
}
void turn_flash_off() { 
    camera_model.isFlashEnabled = false; 
    digitalWrite(FLASH_LED_PIN, LOW); 
    pinMode(FLASH_LED_PIN, INPUT); // Return to high-impedance state to prevent SD line loading
}

// --- Dithering ---
void dither_image(uint8_t* buffer, int width, int height) {
    // 16-bit signed error buffer to avoid overflow during diffusion calculations
    int16_t* err_buf = (int16_t*)malloc(width * height * sizeof(int16_t));
    if (!err_buf) return; // Fail-safe check if heap allocation drops

    // Populate working buffer with raw luminance values
    for (int i = 0; i < width * height; i++) {
        err_buf[i] = buffer[i];
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            int16_t old_pixel = err_buf[idx];
            
            uint8_t new_pixel = (old_pixel < 128) ? 0 : 255;
            buffer[idx] = new_pixel; // Store final bit state back to serialization array
            
            int16_t error = old_pixel - new_pixel;

            // Select Dithering Method
            switch (camera_model.algo) {
                case FLOYD_STEINBERG:
                    if (x + 1 < width)  err_buf[idx + 1]         += (error * 7) / 16;
                    if (y + 1 < height) {
                        if (x - 1 >= 0) err_buf[idx + width - 1] += (error * 3) / 16;
                                        err_buf[idx + width]     += (error * 5) / 16;
                        if (x + 1 < width)  err_buf[idx + width + 1] += (error * 1) / 16;
                    }
                    break;

                case JARVIS_JUDICE_NINKE:
                    if (x + 1 < width)  err_buf[idx + 1] += (error * 7) / 48;
                    if (x + 2 < width)  err_buf[idx + 2] += (error * 5) / 48;
                    if (y + 1 < height) {
                        for (int dx = -2; dx <= 2; dx++) {
                            if (x + dx >= 0 && x + dx < width) {
                                int weights[] = {3, 5, 7, 5, 3};
                                err_buf[idx + width + dx] += (error * weights[dx + 2]) / 48;
                            }
                        }
                    }
                    if (y + 2 < height) {
                        for (int dx = -2; dx <= 2; dx++) {
                            if (x + dx >= 0 && x + dx < width) {
                                int weights[] = {1, 3, 5, 3, 1};
                                err_buf[idx + (2 * width) + dx] += (error * weights[dx + 2]) / 48;
                            }
                        }
                    }
                    break;

                case STUCKI:
                    if (x + 1 < width)  err_buf[idx + 1] += (error * 8) / 42;
                    if (x + 2 < width)  err_buf[idx + 2] += (error * 4) / 42;
                    if (y + 1 < height) {
                        for (int dx = -2; dx <= 2; dx++) {
                            if (x + dx >= 0 && x + dx < width) {
                                int weights[] = {2, 4, 8, 4, 2};
                                err_buf[idx + width + dx] += (error * weights[dx + 2]) / 42;
                            }
                        }
                    }
                    if (y + 2 < height) {
                        for (int dx = -2; dx <= 2; dx++) {
                            if (x + dx >= 0 && x + dx < width) {
                                int weights[] = {1, 2, 4, 2, 1};
                                err_buf[idx + (2 * width) + dx] += (error * weights[dx + 2]) / 42;
                            }
                        }
                    }
                    break;
            }
        }
    }
    free(err_buf); // Clean up memory allocations immediately
}

void initialize_camera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = 5; config.pin_d1 = 18; config.pin_d2 = 19; config.pin_d3 = 21;
    config.pin_d4 = 36; config.pin_d5 = 39; config.pin_d6 = 34; config.pin_d7 = 35;
    config.pin_xclk = 0; config.pin_pclk = 22; config.pin_vsync = 25; config.pin_href = 23;
    config.pin_sscb_sda = 26; config.pin_sscb_scl = 27; config.pin_pwdn = 32; config.pin_reset = -1;
    config.xclk_freq_hz = 20000000;
    
    config.pixel_format = PIXFORMAT_RGB565; 
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) return;
}

// --- Full Color Capture Function ---
void save_full_res_snapshot() {
    if (!sd_card_ready) {
        Serial.println("E:Cannot save, SD card not ready");
        return;
    }

    bool was_streaming = camera_model.isStreamToSerialEnabled;
    camera_model.isStreamToSerialEnabled = false;
    
    Serial.flush(); 
    delay(150);     
    
    Serial.println("I:Flushing streaming buffers...");

    camera_fb_t *flush_fb1 = esp_camera_fb_get();
    if (flush_fb1) esp_camera_fb_return(flush_fb1);
    delay(50);
    camera_fb_t *flush_fb2 = esp_camera_fb_get();
    if (flush_fb2) esp_camera_fb_return(flush_fb2);
    delay(50);

    Serial.flush(); 
    Serial.println("I:Capturing clean RAW high-res snapshot...");

    // Flash
    if (camera_model.isFlashEnabled) {
        pinMode(FLASH_LED_PIN, OUTPUT);
        digitalWrite(FLASH_LED_PIN, HIGH);
        delay(100); // Let the sensor adjust exposure to the light output
    }

    // Grab Frame buffer
    camera_fb_t *fb = esp_camera_fb_get();

    // Deconflict flash pin
    digitalWrite(FLASH_LED_PIN, LOW);
    pinMode(FLASH_LED_PIN, INPUT);

    if (!fb) {
        Serial.println("E:Camera capture failed. Sensor pipeline locked.");
        camera_model.isStreamToSerialEnabled = was_streaming;
        return;
    }

    char filename[32];
    sprintf(filename, "/photo_%u.ppm", photo_counter++);
    
    Serial.flush(); 
    Serial.printf("I:Writing to file: %s\n", filename);

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("E:Failed to open file for writing.");
        esp_camera_fb_return(fb);
        camera_model.isStreamToSerialEnabled = was_streaming;
        return;
    }

    char header[32];
    snprintf(header, sizeof(header), "P6\n%d %d\n255\n", fb->width, fb->height);
    file.write((uint8_t*)header, strlen(header));

    uint16_t* pixel_buffer = (uint16_t*)fb->buf;
    size_t total_pixels = fb->width * fb->height;
    
    size_t bytes_written = 0; 
    size_t expected_bytes = total_pixels * 3;

    for (size_t i = 0; i < total_pixels; i++) {
        uint16_t rgb565_pixel = pixel_buffer[i];
        rgb565_pixel = (rgb565_pixel >> 8) | (rgb565_pixel << 8);

        uint8_t r = ((rgb565_pixel >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((rgb565_pixel >> 5)  & 0x3F) * 255 / 63;
        uint8_t b = (rgb565_pixel & 0x1F)        * 255 / 31;

        bytes_written += file.write(r);
        bytes_written += file.write(g);
        bytes_written += file.write(b);
    }
    file.close();
    esp_camera_fb_return(fb);

    Serial.flush(); 
    if (bytes_written == expected_bytes) {
        Serial.println("I:Successfully saved PPM Color Image!");
    } else {
        Serial.printf("E:Write mismatch! Wrote %d of %d bytes.\n", bytes_written, expected_bytes);
    }

    camera_model.isStreamToSerialEnabled = was_streaming;
}

// --- Command Processor ---
void process_serial_input() {
    if (Serial.available() > 0) {
        char input = Serial.read();
        switch (input) {
            case 'b': lower_brightness(); break;
            case 'B': add_brightness(); break;
            case 'c': lower_contrast(); break;
            case 'C': add_contrast(); break;
            case 'd': set_dithering(false); break;
            case 'D': set_dithering(true); break;
            case 'f': turn_flash_off(); break;
            case 'F': turn_flash_on(); break;
            case 'i': set_inverted(false); break;
            case 'I': set_inverted(true); break;
            
            case 's': camera_model.isStreamToSerialEnabled = false; break;
            case 'S': 
                if (!camera_model.isStreamToSerialEnabled) {
                    camera_model.isStreamToSerialEnabled = true; 
                }
                break;
                
            case '0': set_dithering_algorithm(FLOYD_STEINBERG); break;
            case '1': set_dithering_algorithm(JARVIS_JUDICE_NINKE); break;
            case '2': set_dithering_algorithm(STUCKI); break;
            case 'P': save_full_res_snapshot(); break;
            default: break;
        }
    }
}

// --- Downsampling and Serial Streaming ---
void stream_to_serial() {
    camera_fb_t *frame_buffer = esp_camera_fb_get();
    if (!frame_buffer) return;

    int src_w = frame_buffer->width;
    int src_h = frame_buffer->height;
    
    static uint8_t gray_buf[128 * 128];
    uint16_t* src_buf16 = (uint16_t*)frame_buffer->buf;

    for (uint32_t y = 0; y < 128; y++) {
        uint32_t src_y = (y * src_h) / 128;
        uint32_t src_row_offset = src_y * src_w;
        uint32_t dest_row_offset = y * 128;

        for (uint32_t x = 0; x < 128; x++) {
            uint32_t src_x = (x * src_w) / 128;
            uint16_t rgb565_pixel = src_buf16[src_row_offset + src_x];
            rgb565_pixel = (rgb565_pixel >> 8) | (rgb565_pixel << 8);

            uint8_t r = (rgb565_pixel >> 11) & 0x1F; 
            uint8_t g = (rgb565_pixel >> 5)  & 0x3F; 
            uint8_t b = (rgb565_pixel)       & 0x1F; 

            uint8_t r8 = (r * 255) / 31;
            uint8_t g8 = (g * 255) / 63;
            uint8_t b8 = (b * 255) / 31;

            gray_buf[dest_row_offset + x] = (uint8_t)(0.299f * r8 + 0.587f * g8 + 0.114f * b8);
        }
    }

    if (camera_model.isDitheringEnabled) {
        dither_image(gray_buf, 128, 128);
    }

    for (uint8_t y = 0; y < 128; y++) {
        Serial.print("Y:");      
        Serial.write(y); 

        uint32_t row_offset = y * 128;

        for (uint8_t x = 0; x < 128; x += 8) { 
            uint8_t packed_pixels = 0;
            for (uint8_t bit = 0; bit < 8; ++bit) {
                uint8_t pixel_val = gray_buf[row_offset + x + bit];
                
                if (camera_model.isInvertEnabled) {
                    if (pixel_val > 127) packed_pixels |= (1 << (7 - bit));
                } else {
                    if (pixel_val < 127) packed_pixels |= (1 << (7 - bit));
                }
            }
            Serial.write(packed_pixels); 
        }
    }
    
    Serial.flush(); 
    esp_camera_fb_return(frame_buffer);
}

void setup() {
    set_camera_config_defaults();
    set_camera_model_defaults();
    initialize_camera();
    set_camera_defaults();
    initialize_sd_card(); 
    
    delay(400);

    Serial.begin(230400);
    delay(100); 
    
    // Setup Flash
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    pinMode(FLASH_LED_PIN, INPUT); 

    // Setup SD card
    if(sd_card_ready) {
        Serial.printf("I:System Ready. Next dynamic file index slot: %u\n", photo_counter);
    } else {
        Serial.println("E:System Ready. SD Mount Failed.");
    }
}

void loop() {
    process_serial_input();
    
    // Live execution state handling
    if (camera_model.isStreamToSerialEnabled) {
        // Enforce flash state during live streaming preview
        if (camera_model.isFlashEnabled) {
            pinMode(FLASH_LED_PIN, OUTPUT);
            digitalWrite(FLASH_LED_PIN, HIGH);
        } else {
            digitalWrite(FLASH_LED_PIN, LOW);
            pinMode(FLASH_LED_PIN, INPUT);
        }
        stream_to_serial();
    } else {
        // If not streaming keep flash synchronized to instructions
        if (camera_model.isFlashEnabled) {
            pinMode(FLASH_LED_PIN, OUTPUT);
            digitalWrite(FLASH_LED_PIN, HIGH);
        } else {
            digitalWrite(FLASH_LED_PIN, LOW);
            pinMode(FLASH_LED_PIN, INPUT);
        }
    }
    delay(1); 
}