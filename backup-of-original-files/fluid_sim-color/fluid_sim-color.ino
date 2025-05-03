#include <FastLED.h>
#include <Wire.h>
#include <MPU6050.h>

// Pin definitions
#define LED_PIN     5
#define SDA_PIN     21
#define SCL_PIN     22

#define NUM_LEDS    256
#define MATRIX_WIDTH 16
#define MATRIX_HEIGHT 16
#define FLUID_PARTICLES 64
#define BRIGHTNESS  100

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

// Mutex for synchronization
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// Adjusted constants for smoother motion
const float GRAVITY = 0.3f;    //0.03f
const float DAMPING = 0.60f;   //   0.98f
const float MAX_VELOCITY = 0.7f;  //0.3f
const float MIN_MOVEMENT = 0.001f;  //0.001f

// Function prototypes
void initMPU6050();
void initLEDs();
void initParticles();
void updateParticles();
void drawParticles();
void MPUTask(void *parameter);
void LEDTask(void *parameter);

// Function to convert x,y coordinates to LED index
int xy(int x, int y) {
    x = constrain(x, 0, MATRIX_WIDTH - 1);
    y = constrain(y, 0, MATRIX_HEIGHT - 1);
    return (y & 1) ? (y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x)) : (y * MATRIX_WIDTH + x);
}

void drawParticles() {
    FastLED.clear();
    
    // Create occupancy grid
    bool occupied[MATRIX_WIDTH][MATRIX_HEIGHT] = {{false}};
    
    // Get and smooth gravity direction
    static Vector2D lastGravityDir = {0, 1};
    Vector2D currentGravityDir;
    
    portENTER_CRITICAL(&dataMux);
    currentGravityDir = acceleration;
    portEXIT_CRITICAL(&dataMux);
    
    const float GRAVITY_SMOOTHING = 0.95f;
    lastGravityDir.x = lastGravityDir.x * GRAVITY_SMOOTHING + currentGravityDir.x * (1 - GRAVITY_SMOOTHING);
    lastGravityDir.y = lastGravityDir.y * GRAVITY_SMOOTHING + currentGravityDir.y * (1 - GRAVITY_SMOOTHING);
    
    // Normalize gravity vector
    float gravMagnitude = sqrt(lastGravityDir.x * lastGravityDir.x + lastGravityDir.y * lastGravityDir.y);
    Vector2D gravityDir = {0, 1}; // Default down direction
    
    if (gravMagnitude > 0.1f) {
        gravityDir.x = lastGravityDir.x / gravMagnitude;
        gravityDir.y = lastGravityDir.y / gravMagnitude;
    }

    // Calculate heights and prepare for drawing
    float heights[FLUID_PARTICLES];
    float minHeight = 1000;
    float maxHeight = -1000;
    
    for (int i = 0; i < FLUID_PARTICLES; i++) {
        heights[i] = -(particles[i].position.x * gravityDir.x + 
                      particles[i].position.y * gravityDir.y);
        minHeight = min(minHeight, heights[i]);
        maxHeight = max(maxHeight, heights[i]);
    }
    
    float heightRange = max(maxHeight - minHeight, 1.0f);

    // Draw particles
    int visibleCount = 0;
    for (int i = 0; i < FLUID_PARTICLES; i++) {
        int x = round(constrain(particles[i].position.x, 0, MATRIX_WIDTH - 1));
        int y = round(constrain(particles[i].position.y, 0, MATRIX_HEIGHT - 1));
        
        if (!occupied[x][y]) {
            int index = xy(x, y);
            if (index >= 0 && index < NUM_LEDS) {
                float relativeHeight = (heights[i] - minHeight) / heightRange;
                 uint8_t hue = relativeHeight * 160;  // Map from 0 (red) to 160 (blue)
                  uint8_t sat = 255;  // Full saturation for vibrant colors
                   uint8_t val = 220 + (relativeHeight * 35);  // Slightly brighter at top;
                
                leds[index] = CHSV(hue, sat, val);
                occupied[x][y] = true;
                visibleCount++;
            }
        } else {
            // Find nearest empty position
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    
                    int newX = x + dx;
                    int newY = y + dy;
                    
                    if (newX >= 0 && newX < MATRIX_WIDTH && 
                        newY >= 0 && newY < MATRIX_HEIGHT && 
                        !occupied[newX][newY]) {
                        int index = xy(newX, newY);
                        if (index >= 0 && index < NUM_LEDS) {
                            float relativeHeight = (heights[i] - minHeight) / heightRange;
                        uint8_t hue = relativeHeight * 160;  // Map from 0 (red) to 160 (blue)
                        uint8_t sat = 255;  // Full saturation for vibrant colors
                        uint8_t val = 220 + (relativeHeight * 35);  // Slightly brighter at top

                            
                            leds[index] = CHSV(hue, sat, val);
                            occupied[newX][newY] = true;
                            visibleCount++;
                            goto particleDrawn;
                        }
                    }
                }
            }
            particleDrawn: continue;
        }
    }
    
    // Debug output
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 1000) {
        Serial.printf("Visible particles: %d of %d\n", visibleCount, FLUID_PARTICLES);
        lastDebugTime = millis();
    }
    
    FastLED.show();
}
    


void updateParticles() {
    Vector2D currentAccel;
    portENTER_CRITICAL(&dataMux);
    currentAccel = acceleration;
    portEXIT_CRITICAL(&dataMux);

    // Reduce acceleration sensitivity
    currentAccel.x *= 0.3f;
    currentAccel.y *= 0.3f;

    // Update and constrain each particle
    for (int i = 0; i < FLUID_PARTICLES; i++) {
        // Update velocity with acceleration
        particles[i].velocity.x = particles[i].velocity.x * 0.95f + (currentAccel.x * GRAVITY);
        particles[i].velocity.y = particles[i].velocity.y * 0.95f + (currentAccel.y * GRAVITY);

        // Hard constrain velocity
        particles[i].velocity.x = constrain(particles[i].velocity.x, -MAX_VELOCITY, MAX_VELOCITY);
        particles[i].velocity.y = constrain(particles[i].velocity.y, -MAX_VELOCITY, MAX_VELOCITY);

        // Calculate new position
        float newX = particles[i].position.x + particles[i].velocity.x;
        float newY = particles[i].position.y + particles[i].velocity.y;

        // Strict boundary checking with bounce
        if (newX < 0.0f) {
            newX = 0.0f;
            particles[i].velocity.x = fabs(particles[i].velocity.x) * DAMPING;
        } 
        else if (newX >= (MATRIX_WIDTH - 1.0f)) {
            newX = MATRIX_WIDTH - 1.0f;
            particles[i].velocity.x = -fabs(particles[i].velocity.x) * DAMPING;
        }

        if (newY < 0.0f) {
            newY = 0.0f;
            particles[i].velocity.y = fabs(particles[i].velocity.y) * DAMPING;
        } 
        else if (newY >= (MATRIX_HEIGHT - 1.0f)) {
            newY = MATRIX_HEIGHT - 1.0f;
            particles[i].velocity.y = -fabs(particles[i].velocity.y) * DAMPING;
        }

        // Ensure positions are always within bounds
        particles[i].position.x = constrain(newX, 0.0f, MATRIX_WIDTH - 1.0f);
        particles[i].position.y = constrain(newY, 0.0f, MATRIX_HEIGHT - 1.0f);

        // Additional safety check
        if (isnan(particles[i].position.x) || isnan(particles[i].position.y)) {
            particles[i].position.x = MATRIX_WIDTH / 2;
            particles[i].position.y = MATRIX_HEIGHT / 2;
            particles[i].velocity.x = 0;
            particles[i].velocity.y = 0;
        }
    }

    // Particle collision detection and resolution
    for (int i = 0; i < FLUID_PARTICLES; i++) {
        for (int j = i + 1; j < FLUID_PARTICLES; j++) {
            float dx = particles[j].position.x - particles[i].position.x;
            float dy = particles[j].position.y - particles[i].position.y;
            float distSquared = dx * dx + dy * dy;

            if (distSquared < 1.0f && distSquared > 0.0f) {
                float dist = sqrt(distSquared);
                float nx = dx / dist;
                float ny = dy / dist;

                // Push particles apart
                float pushDistance = (1.0f - dist) * 0.5f;
                float pushX = nx * pushDistance;
                float pushY = ny * pushDistance;

                // Update positions while ensuring they stay in bounds
                particles[i].position.x = constrain(particles[i].position.x - pushX, 0.0f, MATRIX_WIDTH - 1.0f);
                particles[i].position.y = constrain(particles[i].position.y - pushY, 0.0f, MATRIX_HEIGHT - 1.0f);
                particles[j].position.x = constrain(particles[j].position.x + pushX, 0.0f, MATRIX_WIDTH - 1.0f);
                particles[j].position.y = constrain(particles[j].position.y + pushY, 0.0f, MATRIX_HEIGHT - 1.0f);

                // Exchange velocities with damping
                float tempVelX = particles[i].velocity.x;
                float tempVelY = particles[i].velocity.y;
                particles[i].velocity.x = particles[j].velocity.x * DAMPING;
                particles[i].velocity.y = particles[j].velocity.y * DAMPING;
                particles[j].velocity.x = tempVelX * DAMPING;
                particles[j].velocity.y = tempVelY * DAMPING;
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
            Serial.printf("Initializing particle %d at x=%d, y=%d\n", index, x, y);
            particles[index].position = {static_cast<float>(x), static_cast<float>(y)};
            particles[index].velocity = {0.0f, 0.0f};
            index++;
        }
    }
    
    Serial.printf("Total particles initialized: %d\n", index);
    Serial.println("Particles initialized");
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
        updateParticles();
        drawParticles();
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting initialization...");

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