#include <SPI.h>
#include "Adafruit_MAX31855.h"

struct SmaChannel {
  const char *name;
  int pin;
  int pwmChannel;
};

// SMA MOSFET inputs.
// Direction mapping:
//   left  -> GPIO14
//   right -> GPIO16
//   up    -> GPIO25
//   down  -> GPIO26
SmaChannel smaLeft = {"left", 14, 0};
SmaChannel smaRight = {"right", 16, 1};
SmaChannel smaUp = {"up", 25, 2};
SmaChannel smaDown = {"down", 26, 3};

// Thermocouple pins. Keep these unchanged.
const int thermoCLK = 18;
const int thermoCS = 5;
const int thermoDO = 19;
Adafruit_MAX31855 thermocouple(thermoCLK, thermoCS, thermoDO);

// PWM setup for MOSFET control.
const int pwmFreq = 1000;
const int pwmResolution = 8;
const int pwmMax = 255;

// Basic test timings.
const unsigned long DRIVE_MS = 100;           // h/k/u/j/q/e 使用：达到目标温度后保持的时间
const unsigned long PULSE_DRIVE_MS = 500;     // 强刺激：a/d/w/s 主方向驱动 0.5 s
const unsigned long TANGENTIAL_DRIVE_MS = 500; // 强刺激：切向力相邻两路 SMA 同时驱动 0.5 s
const unsigned long TANGENTIAL_RESET_MS = 100; // 强刺激：切向力对角两路 SMA 同时复位 0.1 s
const unsigned long PERCEPTION_HOLD_MS = 1000; // 强刺激：主方向结束后保留 1 s 感知窗口，再复位
const unsigned long F_STEP_DRIVE_MS = 100;    // 只给 f 使用，开环顺/逆时针每一步的驱动时间
const unsigned long F_MIDDLE_PAUSE_MS = 1000; // 只给 f 使用，顺时针和逆时针之间的间隔
const unsigned long LOW_FREQ_STRETCH_MS = 400;       // 低频方向性拉伸：正向拉伸 0.4 s
const unsigned long LOW_FREQ_COOLING_MS = 1000;       // 低频方向性拉伸：正向拉伸后的冷却 1 s
const unsigned long LOW_FREQ_RESET_MS = 100;          // 低频方向性拉伸：反方向复位 0.1 s
const unsigned long LOW_FREQ_TOTAL_RUN_MS = 10000;    // 低频方向性拉伸：固定总运行时间，默认 10 s
const unsigned long RESET_DRIVE_MS = 100;
const unsigned long STEP_GAP_MS = 50;
const unsigned long RECOVERY_WAIT_MS = 3000;

// Safety timings.
const unsigned long EMERGENCY_CHECK_INTERVAL_MS = 5; // 运行中每隔约 5 ms 检查一次 x/X 急停
const unsigned long CLOSED_LOOP_MAX_HEAT_MS = 3500;  // 达到目标触发温度前，单次闭环最长加热时间

// Safety limits for 0.8 A operation.
const int CLOSED_LOOP_PWM_MAX = 80;       // 0.8 A 下限制闭环最大 PWM，先保守使用
const double OVER_TEMP_MARGIN_C = 2.0;    // 测得温度超过目标温度 2 ℃即强制停止
double absoluteTempLimitC = 43.0; // 绝对温度保护上限，上位机联动时最高允许 43 C
const double TARGET_REACHED_MARGIN_C = 1.0; // 热电偶有延迟，测得温度达到目标值前 1 ℃即开始计时

// Temperature closed-loop levels.
// 1/2/3 select the target temperature. h/k/u/j execute that target on a direction.
const double LEVEL_30C_TARGET = 30.0;
const double LEVEL_35C_TARGET = 35.0;
const double LEVEL_40C_TARGET = 40.0;
double activeTargetTemp = LEVEL_30C_TARGET;
const char *activeLevelName = "30C";

enum HostHoldMode {
  HOST_HOLD_NONE,
  HOST_HOLD_SINGLE,
  HOST_HOLD_ALL
};

HostHoldMode hostHoldMode = HOST_HOLD_NONE;
SmaChannel *hostHoldChannel = nullptr;

void stopHostHold();

// PID parameters for 0.8 A operation.
// The previous parameters were too aggressive for 0.8 A because heating power increases greatly.
// Start conservatively: mainly proportional control, no integral accumulation, and PWM capped above.
double Kp = 4.0;
double Ki = 0.0;
double Kd = 0.0;
double integral = 0.0;
double lastError = 0.0;
unsigned long lastPidTime = 0;

const double integralMin = -50.0;
const double integralMax = 50.0;

void writeSma(SmaChannel &channel, int power) {
  ledcWrite(channel.pwmChannel, constrain(power, 0, pwmMax));
}

void allSmaOff() {
  writeSma(smaLeft, 0);
  writeSma(smaRight, 0);
  writeSma(smaUp, 0);
  writeSma(smaDown, 0);
}

bool checkEmergencyStop() {
  if (Serial.available()) {
    char cmd = Serial.peek();

    if (cmd == 'x' || cmd == 'X') {
      Serial.read();
      allSmaOff();
      stopHostHold();
      Serial.println("EMERGENCY STOP: all SMA outputs off");
      return true;
    }
  }

  return false;
}

bool interruptibleDelay(unsigned long durationMs) {
  unsigned long startTime = millis();

  while (millis() - startTime < durationMs) {
    if (checkEmergencyStop()) {
      return false;
    }
    delay(EMERGENCY_CHECK_INTERVAL_MS);
  }

  return true;
}

bool driveChannelFor(SmaChannel &channel, unsigned long durationMs) {
  allSmaOff();
  writeSma(channel, pwmMax);

  if (!interruptibleDelay(durationMs)) {
    allSmaOff();
    return false;
  }

  writeSma(channel, 0);
  return true;
}

bool driveTwoChannelsFor(SmaChannel &channelA, SmaChannel &channelB, unsigned long durationMs) {
  allSmaOff();
  writeSma(channelA, pwmMax);
  writeSma(channelB, pwmMax);

  if (!interruptibleDelay(durationMs)) {
    allSmaOff();
    return false;
  }

  writeSma(channelA, 0);
  writeSma(channelB, 0);
  return true;
}

bool driveAllChannelsFor(unsigned long durationMs) {
  allSmaOff();
  writeSma(smaLeft, pwmMax);
  writeSma(smaRight, pwmMax);
  writeSma(smaUp, pwmMax);
  writeSma(smaDown, pwmMax);

  if (!interruptibleDelay(durationMs)) {
    allSmaOff();
    return false;
  }

  allSmaOff();
  return true;
}

bool driveChannelForPower(SmaChannel &channel, int power, unsigned long durationMs) {
  allSmaOff();
  writeSma(channel, constrain(power, 0, pwmMax));

  if (!interruptibleDelay(durationMs)) {
    allSmaOff();
    return false;
  }

  writeSma(channel, 0);
  return true;
}

bool checkOpenLoopThermalSafety(const char *actionName) {
  double tempC = thermocouple.readCelsius();

  if (isnan(tempC)) {
    Serial.print(actionName);
    Serial.println(": thermocouple read failed, stop output");
    allSmaOff();
    return false;
  }

  if (tempC >= absoluteTempLimitC) {
    Serial.print(actionName);
    Serial.print(": absolute temperature protection stop. Temp ");
    Serial.print(tempC);
    Serial.println(" C");
    allSmaOff();
    return false;
  }

  return true;
}

void resetPidTerms() {
  integral = 0.0;
  lastError = 0.0;
  lastPidTime = millis();
}

bool updateTemperaturePid(SmaChannel &channel, double *currentTempC = nullptr) {
  if (checkEmergencyStop()) {
    return false;
  }

  double tempC = thermocouple.readCelsius();
  if (isnan(tempC)) {
    Serial.println("Thermocouple read failed, stop output");
    allSmaOff();
    return false;
  }

  if (currentTempC != nullptr) {
    *currentTempC = tempC;
  }

  // Extra thermal protection. Because the thermocouple has delay, stop as soon as
  // the measured temperature is only slightly higher than the target.
  if (tempC >= activeTargetTemp + OVER_TEMP_MARGIN_C || tempC >= absoluteTempLimitC) {
    Serial.print("Thermal protection stop. Temp ");
    Serial.print(tempC);
    Serial.print(" C, target ");
    Serial.print(activeTargetTemp);
    Serial.println(" C");
    allSmaOff();
    return false;
  }

  unsigned long now = millis();
  double deltaTime = (now - lastPidTime) / 1000.0;
  if (deltaTime <= 0.0) {
    deltaTime = 0.001;
  }
  lastPidTime = now;

  double error = activeTargetTemp - tempC;

  // For 0.8 A operation, avoid integral windup. Ki is set to 0 by default,
  // but the code is kept here for later tuning if needed.
  if (Ki > 0.0 && fabs(error) < 5.0) {
    integral += error * deltaTime;
    integral = constrain(integral, integralMin, integralMax);
  }

  double derivative = (error - lastError) / deltaTime;
  lastError = error;

  double output = Kp * error + Ki * integral + Kd * derivative;

  // Once the measured temperature reaches the target, stop heating immediately.
  // The hold timer is handled outside this function.
  if (tempC >= activeTargetTemp) {
    output = 0.0;
  }

  output = constrain(output, 0.0, (double)CLOSED_LOOP_PWM_MAX);

  allSmaOff();
  writeSma(channel, (int)output);

  Serial.print("Temp ");
  Serial.print(tempC);
  Serial.print(" C, target ");
  Serial.print(activeTargetTemp);
  Serial.print(" C, pwm ");
  Serial.println(output);

  return true;
}

bool driveChannelClosedLoopFor(SmaChannel &channel, unsigned long durationMs) {
  resetPidTerms();
  allSmaOff();

  bool targetReached = false;
  unsigned long heatStartTime = millis();
  unsigned long holdStartTime = 0;

  while (true) {
    if (checkEmergencyStop()) {
      allSmaOff();
      return false;
    }

    if (!targetReached && millis() - heatStartTime >= CLOSED_LOOP_MAX_HEAT_MS) {
      Serial.println("Closed-loop safety timeout before target reached, stop output");
      allSmaOff();
      return false;
    }

    double tempC = 0.0;
    if (!updateTemperaturePid(channel, &tempC)) {
      allSmaOff();
      return false;
    }

    // Start the fixed-duration timer slightly before the measured temperature
    // reaches the selected target temperature. This compensates for thermocouple delay
    // and reduces SMA overheating risk at 0.8 A.
    if (!targetReached && tempC >= activeTargetTemp - TARGET_REACHED_MARGIN_C) {
      targetReached = true;
      holdStartTime = millis();
      allSmaOff();
      Serial.println("Target trigger reached, start fixed-duration timing");
    }

    if (targetReached && millis() - holdStartTime >= durationMs) {
      break;
    }

    if (!interruptibleDelay(20)) {
      allSmaOff();
      return false;
    }
  }

  allSmaOff();
  return true;
}

void runDirectionTest(SmaChannel &driveChannel, SmaChannel &resetChannel) {
  Serial.print("Drive ");
  Serial.print(driveChannel.name);
  Serial.print(" on GPIO");
  Serial.print(driveChannel.pin);
  Serial.print(", target ");
  Serial.print(activeLevelName);
  Serial.print(", fixed duration after target trigger ");
  Serial.print(DRIVE_MS);
  Serial.println(" ms");

  if (!driveChannelClosedLoopFor(driveChannel, DRIVE_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }

  Serial.print("Perception hold ");
  Serial.print(PERCEPTION_HOLD_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(PERCEPTION_HOLD_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }

  Serial.print("Reset with opposite ");
  Serial.print(resetChannel.name);
  Serial.print(" GPIO");
  Serial.print(resetChannel.pin);
  Serial.print(" for ");
  Serial.print(RESET_DRIVE_MS);
  Serial.println(" ms");

  if (!driveChannelFor(resetChannel, RESET_DRIVE_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }
  allSmaOff();

  Serial.print("Recovery wait ");
  Serial.print(RECOVERY_WAIT_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(RECOVERY_WAIT_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }
  Serial.println("Done");
}

void runPulseDirectionTest(SmaChannel &driveChannel, SmaChannel &resetChannel) {
  Serial.print("Pulse drive ");
  Serial.print(driveChannel.name);
  Serial.print(" on GPIO");
  Serial.print(driveChannel.pin);
  Serial.print(", fixed duration ");
  Serial.print(PULSE_DRIVE_MS);
  Serial.println(" ms");

  if (!driveChannelFor(driveChannel, PULSE_DRIVE_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }

  Serial.print("Perception hold ");
  Serial.print(PERCEPTION_HOLD_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(PERCEPTION_HOLD_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }

  Serial.print("Reset with opposite ");
  Serial.print(resetChannel.name);
  Serial.print(" GPIO");
  Serial.print(resetChannel.pin);
  Serial.print(" for ");
  Serial.print(RESET_DRIVE_MS);
  Serial.println(" ms");

  if (!driveChannelFor(resetChannel, RESET_DRIVE_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }
  allSmaOff();

  Serial.print("Recovery wait ");
  Serial.print(RECOVERY_WAIT_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(RECOVERY_WAIT_MS)) {
    allSmaOff();
    Serial.println("Action aborted");
    return;
  }
  Serial.println("Done");
}

void runTangentialForceTest(
  const char *name,
  SmaChannel &driveChannelA,
  SmaChannel &driveChannelB,
  SmaChannel &resetChannelA,
  SmaChannel &resetChannelB
) {
  Serial.print("Tangential force ");
  Serial.print(name);
  Serial.print(": drive ");
  Serial.print(driveChannelA.name);
  Serial.print("+");
  Serial.print(driveChannelB.name);
  Serial.print(" for ");
  Serial.print(TANGENTIAL_DRIVE_MS);
  Serial.println(" ms");

  if (!driveTwoChannelsFor(driveChannelA, driveChannelB, TANGENTIAL_DRIVE_MS)) {
    allSmaOff();
    Serial.println("Tangential force aborted");
    return;
  }

  Serial.print("Perception hold ");
  Serial.print(PERCEPTION_HOLD_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(PERCEPTION_HOLD_MS)) {
    allSmaOff();
    Serial.println("Tangential force aborted");
    return;
  }

  Serial.print("Reset with opposite ");
  Serial.print(resetChannelA.name);
  Serial.print("+");
  Serial.print(resetChannelB.name);
  Serial.print(" for ");
  Serial.print(TANGENTIAL_RESET_MS);
  Serial.println(" ms");

  if (!driveTwoChannelsFor(resetChannelA, resetChannelB, TANGENTIAL_RESET_MS)) {
    allSmaOff();
    Serial.println("Tangential reset aborted");
    return;
  }

  allSmaOff();
  Serial.println("Tangential force done");
}

void runLowFrequencyDirectionalStretch(SmaChannel &stretchChannel, SmaChannel &resetChannel) {
  Serial.print("Low-frequency directional stretch ");
  Serial.print(stretchChannel.name);
  Serial.print(" GPIO");
  Serial.print(stretchChannel.pin);
  Serial.print(" -> cooling -> reset with opposite ");
  Serial.print(resetChannel.name);
  Serial.print(" GPIO");
  Serial.print(resetChannel.pin);
  Serial.print(", total run ");
  Serial.print(LOW_FREQ_TOTAL_RUN_MS);
  Serial.println(" ms");

  const unsigned long lowFreqCycleMs = LOW_FREQ_STRETCH_MS + LOW_FREQ_COOLING_MS + LOW_FREQ_RESET_MS;
  unsigned long startTime = millis();
  int cycleCount = 0;
  bool finalResetRequired = false;

  // A full low-frequency cycle is: stretch -> cooling -> reset.
  // Do not start a new cycle unless there is enough remaining time to finish the reset.
  while (millis() - startTime + lowFreqCycleMs <= LOW_FREQ_TOTAL_RUN_MS) {
    cycleCount++;
    Serial.print("Low-frequency cycle ");
    Serial.println(cycleCount);

    if (!checkOpenLoopThermalSafety("Low-frequency stretch before drive")) {
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    Serial.print("Stretch ");
    Serial.print(stretchChannel.name);
    Serial.print(" for ");
    Serial.print(LOW_FREQ_STRETCH_MS);
    Serial.println(" ms");

    // Use the same open-loop output as a/d/w/s pulse commands:
    // driveChannelFor() writes pwmMax to the selected channel.
    if (!driveChannelFor(stretchChannel, LOW_FREQ_STRETCH_MS)) {
      allSmaOff();
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    allSmaOff();
    finalResetRequired = true;

    if (!checkOpenLoopThermalSafety("Low-frequency stretch after drive")) {
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    Serial.print("Cooling wait ");
    Serial.print(LOW_FREQ_COOLING_MS);
    Serial.println(" ms");
    if (!interruptibleDelay(LOW_FREQ_COOLING_MS)) {
      allSmaOff();
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    if (!checkOpenLoopThermalSafety("Low-frequency stretch before reset")) {
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    Serial.print("Reset with opposite ");
    Serial.print(resetChannel.name);
    Serial.print(" for ");
    Serial.print(LOW_FREQ_RESET_MS);
    Serial.println(" ms");

    if (!driveChannelFor(resetChannel, LOW_FREQ_RESET_MS)) {
      allSmaOff();
      Serial.println("Low-frequency stretch aborted");
      return;
    }

    allSmaOff();
    finalResetRequired = false;

    if (!checkOpenLoopThermalSafety("Low-frequency stretch after reset")) {
      Serial.println("Low-frequency stretch aborted");
      return;
    }
  }

  // End state protection: if the function ever leaves a stretch cycle before reset,
  // perform one final opposite-direction reset before normal completion.
  if (finalResetRequired) {
    Serial.print("Final reset with opposite ");
    Serial.print(resetChannel.name);
    Serial.print(" for ");
    Serial.print(LOW_FREQ_RESET_MS);
    Serial.println(" ms");

    if (!driveChannelFor(resetChannel, LOW_FREQ_RESET_MS)) {
      allSmaOff();
      Serial.println("Low-frequency final reset aborted");
      return;
    }
    allSmaOff();
  }

  Serial.print("Low-frequency stretch done, cycles = ");
  Serial.println(cycleCount);
}

void driveSequence(const char *name, SmaChannel *sequence[], int count) {
  Serial.print("Run sequence: ");
  Serial.println(name);

  for (int i = 0; i < count; i++) {
    Serial.print(sequence[i]->name);
    Serial.print(" GPIO");
    Serial.print(sequence[i]->pin);
    Serial.print(", target ");
    Serial.print(activeLevelName);
    Serial.print(", fixed duration after target trigger ");
    Serial.print(DRIVE_MS);
    Serial.println(" ms");

    if (!driveChannelClosedLoopFor(*sequence[i], DRIVE_MS)) {
      allSmaOff();
      Serial.println("Sequence aborted");
      return;
    }

    if (!interruptibleDelay(STEP_GAP_MS)) {
      allSmaOff();
      Serial.println("Sequence aborted");
      return;
    }
  }

  allSmaOff();
  Serial.println("Sequence done");
}

void runClockwise() {
  SmaChannel *sequence[] = {&smaUp, &smaRight, &smaDown, &smaLeft};
  driveSequence("clockwise: up -> right -> down -> left", sequence, 4);
}

void runCounterClockwise() {
  SmaChannel *sequence[] = {&smaUp, &smaLeft, &smaDown, &smaRight};
  driveSequence("counter-clockwise: up -> left -> down -> right", sequence, 4);
}

bool driveOpenLoopSequence(const char *name, SmaChannel *sequence[], int count) {
  Serial.print("Run open-loop sequence: ");
  Serial.println(name);

  for (int i = 0; i < count; i++) {
    Serial.print(sequence[i]->name);
    Serial.print(" GPIO");
    Serial.print(sequence[i]->pin);
    Serial.print(", open-loop duration ");
    Serial.print(F_STEP_DRIVE_MS);
    Serial.println(" ms");

    if (!driveChannelFor(*sequence[i], F_STEP_DRIVE_MS)) {
      allSmaOff();
      Serial.println("Open-loop sequence aborted");
      return false;
    }

    if (!interruptibleDelay(STEP_GAP_MS)) {
      allSmaOff();
      Serial.println("Open-loop sequence aborted");
      return false;
    }
  }

  allSmaOff();
  Serial.println("Open-loop sequence done");
  return true;
}

void runDropRecovery() {
  Serial.println("Drop/loss recovery: open-loop clockwise -> 1s pause -> open-loop counter-clockwise");

  SmaChannel *clockwiseSequence[] = {&smaUp, &smaRight, &smaDown, &smaLeft};
  SmaChannel *counterClockwiseSequence[] = {&smaUp, &smaLeft, &smaDown, &smaRight};

  if (!driveOpenLoopSequence("clockwise: up -> right -> down -> left", clockwiseSequence, 4)) {
    allSmaOff();
    Serial.println("Drop/loss recovery aborted");
    return;
  }

  Serial.print("Middle pause ");
  Serial.print(F_MIDDLE_PAUSE_MS);
  Serial.println(" ms");
  if (!interruptibleDelay(F_MIDDLE_PAUSE_MS)) {
    allSmaOff();
    Serial.println("Drop/loss recovery aborted");
    return;
  }

  if (!driveOpenLoopSequence("counter-clockwise: up -> left -> down -> right", counterClockwiseSequence, 4)) {
    allSmaOff();
    Serial.println("Drop/loss recovery aborted");
    return;
  }

  allSmaOff();
  Serial.println("Drop/loss recovery done");
}

void setTemperatureLevel(const char *levelName, double targetTemp) {
  activeLevelName = levelName;
  activeTargetTemp = targetTemp;

  Serial.print("Selected temperature target ");
  Serial.print(activeLevelName);
  Serial.print(" = ");
  Serial.print(activeTargetTemp);
  Serial.println(" C");
}

void printTemperature() {
  double tempC = thermocouple.readCelsius();

  if (isnan(tempC)) {
    Serial.println("Thermocouple read failed");
    return;
  }

  Serial.print("Temperature: ");
  Serial.print(tempC);
  Serial.println(" C");
}

SmaChannel *channelFromHostCode(char code) {
  switch (code) {
    case 'L':
      return &smaLeft;
    case 'R':
      return &smaRight;
    case 'U':
      return &smaUp;
    case 'D':
      return &smaDown;
    default:
      return nullptr;
  }
}

void stopHostHold() {
  hostHoldMode = HOST_HOLD_NONE;
  hostHoldChannel = nullptr;
  allSmaOff();
}

bool updateTemperaturePidAll(double *currentTempC = nullptr) {
  if (checkEmergencyStop()) {
    return false;
  }

  double tempC = thermocouple.readCelsius();
  if (isnan(tempC)) {
    Serial.println("Thermocouple read failed, stop all output");
    allSmaOff();
    return false;
  }

  if (currentTempC != nullptr) {
    *currentTempC = tempC;
  }

  if (tempC >= activeTargetTemp + OVER_TEMP_MARGIN_C || tempC >= absoluteTempLimitC) {
    Serial.print("Thermal protection stop. Temp ");
    Serial.print(tempC);
    Serial.print(" C, target ");
    Serial.print(activeTargetTemp);
    Serial.println(" C");
    stopHostHold();
    return false;
  }

  unsigned long now = millis();
  double deltaTime = (now - lastPidTime) / 1000.0;
  if (deltaTime <= 0.0) {
    deltaTime = 0.001;
  }
  lastPidTime = now;

  double error = activeTargetTemp - tempC;
  double derivative = (error - lastError) / deltaTime;
  lastError = error;

  double output = Kp * error + Ki * integral + Kd * derivative;
  if (tempC >= activeTargetTemp) {
    output = 0.0;
  }
  output = constrain(output, 0.0, (double)CLOSED_LOOP_PWM_MAX);

  allSmaOff();
  writeSma(smaLeft, (int)output);
  writeSma(smaRight, (int)output);
  writeSma(smaUp, (int)output);
  writeSma(smaDown, (int)output);

  Serial.print("Temp ");
  Serial.print(tempC);
  Serial.print(" C, target ");
  Serial.print(activeTargetTemp);
  Serial.print(" C, all pwm ");
  Serial.println(output);
  return true;
}

void updateHostHold() {
  if (hostHoldMode == HOST_HOLD_NONE) {
    return;
  }

  if (hostHoldMode == HOST_HOLD_ALL) {
    if (!updateTemperaturePidAll()) {
      stopHostHold();
    }
  } else if (hostHoldChannel != nullptr) {
    if (!updateTemperaturePid(*hostHoldChannel)) {
      stopHostHold();
    }
  }
}

void handleHostCommand() {
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) {
    return;
  }

  char action = line.charAt(0);

  if (action == 'S') {
    stopHostHold();
    Serial.println("HOST STOP");
    return;
  }

  if (action == 'T') {
    double tempC = thermocouple.readCelsius();
    if (isnan(tempC)) {
      Serial.println("TEMP:nan");
    } else {
      Serial.print("TEMP:");
      Serial.println(tempC);
    }
    return;
  }

  if (action == 'A') {
    double limit = line.substring(1).toDouble();
    if (limit < 30.0) {
      limit = 30.0;
    }
    if (limit > 43.0) {
      limit = 43.0;
    }
    absoluteTempLimitC = limit;
    Serial.print("HOST ALARM TEMP ");
    Serial.println(absoluteTempLimitC);
    return;
  }

  if (line.length() < 2) {
    Serial.println("HOST ERR");
    return;
  }

  char dir = line.charAt(1);
  SmaChannel *channel = channelFromHostCode(dir);

  if (action == 'O') {
    stopHostHold();
    if (dir == 'F') {
      driveAllChannelsFor(PULSE_DRIVE_MS);
    } else if (channel != nullptr) {
      driveChannelFor(*channel, PULSE_DRIVE_MS);
    }
    allSmaOff();
    Serial.println("HOST OPEN DONE");
    return;
  }

  if (action == 'H') {
    double target = line.substring(2).toDouble();
    if (target <= 0.0) {
      target = LEVEL_35C_TARGET;
    }
    target = constrain(target, 25.0, 40.0);
    activeTargetTemp = target;
    resetPidTerms();

    if (dir == 'F') {
      hostHoldMode = HOST_HOLD_ALL;
      hostHoldChannel = nullptr;
    } else if (channel != nullptr) {
      hostHoldMode = HOST_HOLD_SINGLE;
      hostHoldChannel = channel;
    } else {
      Serial.println("HOST ERR");
      return;
    }

    Serial.print("HOST HOLD ");
    Serial.print(dir);
    Serial.print(" ");
    Serial.println(activeTargetTemp);
    return;
  }

  Serial.println("HOST ERR");
}

void printHelp() {
  Serial.println();
  Serial.println("SMA serial rule test ready");
  Serial.println("1: select 30C target");
  Serial.println("2: select 35C target");
  Serial.println("3: select 40C target");
  Serial.println("h: closed-loop left  GPIO14, then reset GPIO16");
  Serial.println("k: closed-loop right GPIO16, then reset GPIO14");
  Serial.println("u: closed-loop up    GPIO25, then reset GPIO26");
  Serial.println("j: closed-loop down  GPIO26, then reset GPIO25");
  Serial.println("a: pulse left  GPIO14 0.5s -> hold 1s -> reset GPIO16 0.1s");
  Serial.println("d: pulse right GPIO16 0.5s -> hold 1s -> reset GPIO14 0.1s");
  Serial.println("w: pulse up    GPIO25 0.5s -> hold 1s -> reset GPIO26 0.1s");
  Serial.println("s: pulse down  GPIO26 0.5s -> hold 1s -> reset GPIO25 0.1s");
  Serial.println("r: tangential left+up    GPIO14+GPIO25 0.5s -> hold 1s -> reset GPIO16+GPIO26 0.1s");
  Serial.println("v: tangential right+up   GPIO16+GPIO25 0.5s -> hold 1s -> reset GPIO14+GPIO26 0.1s");
  Serial.println("b: tangential left+down  GPIO14+GPIO26 0.5s -> hold 1s -> reset GPIO16+GPIO25 0.1s");
  Serial.println("n: tangential right+down GPIO16+GPIO26 0.5s -> hold 1s -> reset GPIO14+GPIO25 0.1s");
  Serial.println("z: low-frequency stretch left  GPIO14 0.4s -> cool 1s -> reset GPIO16 0.1s, ends after reset");
  Serial.println("c: low-frequency stretch right GPIO16 0.4s -> cool 1s -> reset GPIO14 0.1s, ends after reset");
  Serial.println("i: low-frequency stretch up    GPIO25 0.4s -> cool 1s -> reset GPIO26 0.1s, ends after reset");
  Serial.println("m: low-frequency stretch down  GPIO26 0.4s -> cool 1s -> reset GPIO25 0.1s, ends after reset");
  Serial.println("q: clockwise sequence, up -> right -> down -> left");
  Serial.println("e: counter-clockwise sequence, up -> left -> down -> right");
  Serial.println("f: open-loop drop/loss recovery, clockwise -> 1s pause -> counter-clockwise");
  Serial.println("t: read thermocouple");
  Serial.println("x: EMERGENCY STOP / all SMA off");
  Serial.println("?: help");
  Serial.println();
}

void setupSmaChannel(SmaChannel &channel) {
  pinMode(channel.pin, OUTPUT);
  ledcSetup(channel.pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(channel.pin, channel.pwmChannel);
  writeSma(channel, 0);
}

void setup() {
  Serial.begin(115200);

  setupSmaChannel(smaLeft);
  setupSmaChannel(smaRight);
  setupSmaChannel(smaUp);
  setupSmaChannel(smaDown);
  allSmaOff();

  delay(500);
  printHelp();
}

void loop() {
  updateHostHold();

  if (!Serial.available()) {
    delay(hostHoldMode == HOST_HOLD_NONE ? 10 : 20);
    return;
  }

  char cmd = Serial.read();

  switch (cmd) {
    case '@':
      handleHostCommand();
      break;

    case '1':
      setTemperatureLevel("30C", LEVEL_30C_TARGET);
      break;

    case '2':
      setTemperatureLevel("35C", LEVEL_35C_TARGET);
      break;

    case '3':
      setTemperatureLevel("40C", LEVEL_40C_TARGET);
      break;

    case 'h':
    case 'H':
      runDirectionTest(smaLeft, smaRight);
      break;

    case 'k':
    case 'K':
      runDirectionTest(smaRight, smaLeft);
      break;

    case 'u':
    case 'U':
      runDirectionTest(smaUp, smaDown);
      break;

    case 'j':
    case 'J':
      runDirectionTest(smaDown, smaUp);
      break;

    case 'a':
    case 'A':
      runPulseDirectionTest(smaLeft, smaRight);
      break;

    case 'd':
    case 'D':
      runPulseDirectionTest(smaRight, smaLeft);
      break;

    case 'w':
    case 'W':
      runPulseDirectionTest(smaUp, smaDown);
      break;

    case 's':
    case 'S':
      runPulseDirectionTest(smaDown, smaUp);
      break;

    case 'r':
    case 'R':
      runTangentialForceTest("left+up", smaLeft, smaUp, smaRight, smaDown);
      break;

    case 'v':
    case 'V':
      runTangentialForceTest("right+up", smaRight, smaUp, smaLeft, smaDown);
      break;

    case 'b':
    case 'B':
      runTangentialForceTest("left+down", smaLeft, smaDown, smaRight, smaUp);
      break;

    case 'n':
    case 'N':
      runTangentialForceTest("right+down", smaRight, smaDown, smaLeft, smaUp);
      break;

    case 'z':
    case 'Z':
      runLowFrequencyDirectionalStretch(smaLeft, smaRight);
      break;

    case 'c':
    case 'C':
      runLowFrequencyDirectionalStretch(smaRight, smaLeft);
      break;

    case 'i':
    case 'I':
      runLowFrequencyDirectionalStretch(smaUp, smaDown);
      break;

    case 'm':
    case 'M':
      runLowFrequencyDirectionalStretch(smaDown, smaUp);
      break;

    case 'q':
    case 'Q':
      runClockwise();
      break;

    case 'e':
    case 'E':
      runCounterClockwise();
      break;

    case 'f':
    case 'F':
      runDropRecovery();
      break;

    case 't':
    case 'T':
      printTemperature();
      break;

    case 'x':
    case 'X':
      allSmaOff();
      Serial.println("EMERGENCY STOP: all SMA outputs off");
      break;

    case '?':
      printHelp();
      break;

    case '\n':
    case '\r':
      break;

    default:
      Serial.println("Unknown command");
      printHelp();
      break;
  }
}
