  // colors:   0 = Red, 32 = Orange, 64 = Yellow, 96 = Green, 128 = Aqua, 160 = Blue, 192 = Purple, 224 = Pink


#include <FastLED.h>
#include <Wire.h>
#include <MPU6050.h>

// Pin definitions
#define LED_PIN     5
#define SDA_PIN     21
#define SCL_PIN     22
#define BUTTON_PIN  4   // Button for color switching

#define NUM_LEDS    256
#define MATRIX_WIDTH 16
#define MATRIX_HEIGHT 16
#define FLUID_PARTICLES 64  //80/64
#define BRIGHTNESS  30
#define NUM_COLORS  3   // Number of color options

// Structures
struct Vector2D {
    float x;
    float y;
};

struct Particle {
    Vector2D position;
    Vector2D velocity;
};

// Global variables
CRGB leds[NUM_LEDS];
MPU6050 mpu;
Particle particles[FLUID_PARTICLES];
Vector2D acceleration = {0, 0};

// Color switching variables
uint8_t currentColorIndex = 0;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

// Define the colors (you can change these hue values)
const uint8_t COLORS[NUM_COLORS] = {
    160,  // Blue
    0,    // Red
    96    // Green
};

// Mutex for synchronization
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// Constants for physics
const float GRAVITY = 0.08f;  //0.3f /098f
const float DAMPING = 0.92f;   //0.99f /0.9f
const float MAX_VELOCITY = 0.6f; //0.6f /2.9f

// Function prototypes
void initMPU6050();
void initLEDs();
void initParticles();
void updateParticles();
void drawParticles();
void MPUTask(void *parameter);
void LEDTask(void *parameter);
void checkButton();

// Function to convert x,y coordinates to LED index
int xy(int x, int y) {
    x = constrain(x, 0, MATRIX_WIDTH - 1);
    y = constrain(y, 0, MATRIX_HEIGHT - 1);
    return (y & 1) ? (y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)) : (y * MATRIX_WIDTH + x);
}

void checkButton() {
    static bool lastButtonState = HIGH;
    bool buttonState = digitalRead(BUTTON_PIN);

    if (buttonState == LOW && lastButtonState == HIGH) {  // Button pressed
        if ((millis() - lastDebounceTime) > debounceDelay) {
            currentColorIndex = (currentColorIndex + 1) % NUM_COLORS;
            lastDebounceTime = millis();
        }
    }
    lastButtonState = buttonState;
}

void drawParticles() {
    FastLED.clear();
    
    bool occupied[MATRIX_WIDTH][MATRIX_HEIGHT] = {{false}};
    
    struct ParticleIndex {
        int index;
        float position;
    };
    
    ParticleIndex sortedParticles[FLUID_PARTICLES];
    for (int i = 0; i < FLUID_PARTICLES; i++) {
        sortedParticles[i].index = i;
        sortedParticles[i].position = particles[i].position.y * MATRIX_WIDTH + particles[i].position.x;
    }
    
    for (int i = 0; i < FLUID_PARTICLES - 1; i++) {
        for (int j = 0; j < FLUID_PARTICLES - i - 1; j++) {
            if (sortedParticles[j].position > sortedParticles[j + 1].position) {
                ParticleIndex temp = sortedParticles[j];
                sortedParticles[j] = sortedParticles[j + 1];
                sortedParticles[j + 1] = temp;
            }
        }
    }

    for (int i = 0; i < FLUID_PARTICLES; i++) {
        int particleIndex = sortedParticles[i].index;
        int x = round(particles[particleIndex].position.x);
        int y = round(particles[particleIndex].position.y);
        
        x = constrain(x, 0, MATRIX_WIDTH - 1);
        y = constrain(y, 0, MATRIX_HEIGHT - 1);
        
        if (!occupied[x][y]) {
            int index = xy(x, y);
            if (index >= 0 && index < NUM_LEDS) {
                float speed = sqrt(
                    particles[particleIndex].velocity.x * particles[particleIndex].velocity.x + 
                    particles[particleIndex].velocity.y * particles[particleIndex].velocity.y
                );
                
                uint8_t hue = COLORS[currentColorIndex];
                uint8_t sat = 255;
                uint8_t val = constrain(180 + (speed * 50), 180, 255);
                
                leds[index] = CHSV(hue, sat, val);
                occupied[x][y] = true;
            }
        } else {
            for (int r = 1; r < 3; r++) {
                for (int dx = -r; dx <= r; dx++) {
                    for (int dy = -r; dy <= r; dy++) {
                        if (abs(dx) + abs(dy) == r) {
                            int newX = x + dx;
                            int newY = y + dy;
                            if (newX >= 0 && newX < MATRIX_WIDTH && 
                                newY >= 0 && newY < MATRIX_HEIGHT && 
                                !occupied[newX][newY]) {
                                int index = xy(newX, newY);
                                if (index >= 0 && index < NUM_LEDS) {
                                    leds[index] = CHSV(COLORS[currentColorIndex], 255, 180);
                                    occupied[newX][newY] = true;
                                    goto nextParticle;
                                }
                            }
                        }
                    }
                }
            }
            nextParticle:
            continue;
        }
    }
    
    FastLED.show();
}

void updateParticles() {
    Vector2D currentAccel;
    portENTER_CRITICAL(&dataMux);
    currentAccel = acceleration;
    portEXIT_CRITICAL(&dataMux);

    currentAccel.x *= 0.3f;
    currentAccel.y *= 0.3f;

    for (int i = 0; i < FLUID_PARTICLES; i++) {
        particles[i].velocity.x = particles[i].velocity.x * 0.9f + (currentAccel.x * GRAVITY);
        particles[i].velocity.y = particles[i].velocity.y * 0.9f + (currentAccel.y * GRAVITY);

        particles[i].velocity.x = constrain(particles[i].velocity.x, -MAX_VELOCITY, MAX_VELOCITY);
        particles[i].velocity.y = constrain(particles[i].velocity.y, -MAX_VELOCITY, MAX_VELOCITY);

        float newX = particles[i].position.x + particles[i].velocity.x;
        float newY = particles[i].position.y + particles[i].velocity.y;

        if (newX < 0.0f) {
            newX = 0.0f;
            particles[i].velocity.x = fabs(particles[i].velocity.x) * DAMPING;
        } 
        else if (newX >= (MATRIX_WIDTH - 1)) {
            newX = MATRIX_WIDTH - 1;
            particles[i].velocity.x = -fabs(particles[i].velocity.x) * DAMPING;
        }

        if (newY < 0.0f) {
            newY = 0.0f;
            particles[i].velocity.y = fabs(particles[i].velocity.y) * DAMPING;
        } 
        else if (newY >= (MATRIX_HEIGHT - 1)) {
            newY = MATRIX_HEIGHT - 1;
            particles[i].velocity.y = -fabs(particles[i].velocity.y) * DAMPING;
        }

        particles[i].position.x = constrain(newX, 0.0f, MATRIX_WIDTH - 1);
        particles[i].position.y = constrain(newY, 0.0f, MATRIX_HEIGHT - 1);

        particles[i].velocity.x *= 0.95f;
        particles[i].velocity.y *= 0.95f;
    }

    for (int i = 0; i < FLUID_PARTICLES; i++) {
        for (int j = i + 1; j < FLUID_PARTICLES; j++) {
            float dx = particles[j].position.x - particles[i].position.x;
            float dy = particles[j].position.y - particles[i].position.y;
            float distanceSquared = dx * dx + dy * dy;

            if (distanceSquared < 1.0f) {
                float distance = sqrt(distanceSquared);
                float angle = atan2(dy, dx);
                
                float repulsionX = cos(angle) * 0.5f;
                float repulsionY = sin(angle) * 0.5f;

                particles[i].position.x -= repulsionX * 0.3f;
                particles[i].position.y -= repulsionY * 0.3f;
                particles[j].position.x += repulsionX * 0.3f;
                particles[j].position.y += repulsionY * 0.3f;

                Vector2D avgVel = {
                    (particles[i].velocity.x + particles[j].velocity.x) * 0.5f,
                    (particles[i].velocity.y + particles[j].velocity.y) * 0.5f
                };

                particles[i].velocity = avgVel;
                particles[j].velocity = avgVel;
            }
        }
    }
}

void initMPU6050() {
    Serial.println("Initializing MPU6050...");
    mpu.initialize();
    
    if (!mpu.testConnection()) {
        Serial.println("MPU6050 connection failed!");
        while (1) {
            delay(100);
        }
    }
    
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
    Serial.println("MPU6050 initialized");
}

void initLEDs() {
    Serial.println("Initializing LEDs...");
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear(true);
    Serial.println("LEDs initialized");
}

void initParticles() {
    Serial.println("Initializing particles...");
    int index = 0;
    
    for (int y = MATRIX_HEIGHT - 4; y < MATRIX_HEIGHT; y++) {
        for (int x = 0; x < MATRIX_WIDTH && index < FLUID_PARTICLES; x++) {
            particles[index].position = {static_cast<float>(x), static_cast<float>(y)};
            particles[index].velocity = {0.0f, 0.0f};
            index++;
        }
    }
    
    Serial.printf("Total particles initialized: %d\n", index);
}

void MPUTask(void *parameter) {
    while (true) {
        int16_t ax, ay, az;
        mpu.getAcceleration(&ax, &ay, &az);
         
        portENTER_CRITICAL(&dataMux);
        acceleration.x = -constrain(ax / 16384.0f, -1.0f, 1.0f);
        acceleration.y = constrain(ay / 16384.0f, -1.0f, 1.0f);
        portEXIT_CRITICAL(&dataMux);
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void LEDTask(void *parameter) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(16);
    
    while (true) {
        checkButton();
        updateParticles();
        drawParticles();
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting initialization...");

    // Initialize button pin
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

    initMPU6050();
    initLEDs();
    initParticles();

    xTaskCreatePinnedToCore(
        MPUTask,
        "MPUTask",
        4096,
        NULL,
        2,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        LEDTask,
        "LEDTask",
        4096,
        NULL,
        1,
        NULL,
        1
    );

    Serial.println("Setup complete");
}

void loop() {
    vTaskDelete(NULL);
}