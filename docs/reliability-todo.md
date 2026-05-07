# Keg Washer Reliability Improvements TODO

This document outlines the planned reliability improvements for the Keg Washer Control System, organized by priority and module.

## Priority 1: Hardware Interface Improvements (KegHardware.cpp)

Hardware interfaces are the most common failure points in control systems. Improvements here provide the biggest reliability gains.

- [ ] **Input debouncing**
  - Implement debounce function for all digital inputs
  - Apply to cycle start, manual drain, and keg size switches
  - Example implementation:
    ```cpp
    bool readInputWithDebounce(int pin, int debounceDelay = 50) {
      static unsigned long lastDebounceTime[20] = {0}; // Array for multiple pins
      static int lastReading[20] = {0};
      static int stableState[20] = {0};
      int pinIndex = pin % 20; // Simple hash
      
      int reading = digitalRead(pin);
      if (reading != lastReading[pinIndex]) {
        lastDebounceTime[pinIndex] = millis();
      }
      
      if ((millis() - lastDebounceTime[pinIndex]) > debounceDelay) {
        if (reading != stableState[pinIndex]) {
          stableState[pinIndex] = reading;
        }
      }
      
      lastReading[pinIndex] = reading;
      return stableState[pinIndex];
    }
    ```

- [ ] **Sensor validation and filtering**
  - Add min/max bounds checking for temperature sensors
  - Implement moving average filter for temperature readings
  - Add sensor failure detection
  - Example:
    ```cpp
    int getFilteredTemperature(int sensorValue, int &lastValue) {
      // Check for sensor failure
      if (sensorValue < 0 || sensorValue > 4095) {
        // Use previous valid reading or default
        return lastValue;
      }
      
      // Simple low-pass filter
      lastValue = (lastValue * 3 + sensorValue) / 4;
      return lastValue;
    }
    ```

- [ ] **Output verification**
  - Add feedback checking for critical outputs when possible
  - Add timeouts for expected state changes
  - Example:
    ```cpp
    bool activateOutputWithVerification(int outputPin, int feedbackPin, int timeoutMs) {
      digitalWrite(outputPin, HIGH);
      
      // Wait for feedback with timeout
      unsigned long startTime = millis();
      while (digitalRead(feedbackPin) != HIGH) {
        if (millis() - startTime > timeoutMs) {
          // Timeout occurred, operation failed
          return false;
        }
        delay(1);
      }
      return true;
    }
    ```

- [ ] **Redundant sensors**
  - Add dual temperature sensors for critical measurements
  - Implement voting logic for sensor readings
  - Example:
    ```cpp
    int getRedundantTemperature(int sensor1, int sensor2) {
      int temp1 = map(analogRead(sensor1), 0, 4095, 0, 100);
      int temp2 = map(analogRead(sensor2), 0, 4095, 0, 100);
      
      // If readings are close enough, average them
      if (abs(temp1 - temp2) < 10) {
        return (temp1 + temp2) / 2;
      }
      // Otherwise use the more conservative reading
      return min(temp1, temp2);
    }
    ```

## Priority 2: System Stability (KegWasher.ino)

Overall system stability ensures the control system can recover from unexpected conditions.

- [ ] **Watchdog timer implementation**
  - Enable hardware watchdog timer to reset system when frozen
  - Example:
    ```cpp
    #include <avr/wdt.h>
    
    void setup() {
      // Other initialization
      
      // Setup watchdog with 8-second timeout
      wdt_enable(WDTO_8S);
    }
    
    void loop() {
      // Reset watchdog at each loop iteration
      wdt_reset();
      
      // Rest of the loop code
    }
    ```

- [ ] **Power failure recovery**
  - Store current state in EEPROM
  - Implement recovery logic on startup
  - Example:
    ```cpp
    #include <EEPROM.h>
    
    #define EEPROM_STATE_ADDR 0
    #define EEPROM_VALID_FLAG 42
    
    void saveStateToEEPROM() {
      EEPROM.update(EEPROM_STATE_ADDR, currentState);
      EEPROM.update(EEPROM_STATE_ADDR + 1, EEPROM_VALID_FLAG);
    }
    
    void recoverStateFromEEPROM() {
      if (EEPROM.read(EEPROM_STATE_ADDR + 1) == EEPROM_VALID_FLAG) {
        byte savedState = EEPROM.read(EEPROM_STATE_ADDR);
        // Validate the saved state is reasonable
        if (savedState < NUM_STATES) {
          // Ask user if they want to continue from previous state
          display_showMessage("Continue from previous cycle?");
          // Wait for user input...
        }
      }
    }
    ```

- [ ] **Exception handling**
  - Add try/catch blocks for non-trivial operations
  - Implement safe mode when critical errors occur
  - Example:
    ```cpp
    bool performCriticalOperation() {
      try {
        // Critical operation code
        return true;
      } catch (...) {
        // Log the error
        diagnostics_logEvent("Critical operation failed");
        // Enter safe mode
        hardware_allStop();
        return false;
      }
    }
    ```

- [ ] **System health monitoring**
  - Regular checks of all subsystems
  - CPU load monitoring
  - Memory usage tracking
  - Example:
    ```cpp
    void checkSystemHealth() {
      // Check free RAM
      extern int __heap_start, *__brkval;
      int freeMemory = (int) &freeMemory - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
      
      if (freeMemory < 512) {
        // Memory is getting low, log warning
        diagnostics_logEvent("Low memory");
      }
      
      // Check loop execution time
      static unsigned long lastLoopTime = 0;
      unsigned long loopTime = millis() - lastLoopTime;
      lastLoopTime = millis();
      
      if (loopTime > 100) {
        // Loop is taking too long, log warning
        diagnostics_logEvent("Loop slowdown");
      }
    }
    ```

## Priority 3: Configuration Robustness (KegConfig.cpp)

Ensuring configuration is valid prevents many operational issues.

- [ ] **Configuration validation**
  - Add min/max range checks for all configuration values
  - Implement validation for each parameter individually
  - Example:
    ```cpp
    bool validateConfigValue(const char* key, const char* value) {
      if (strcmp(key, "washTimer") == 0) {
        long intValue = atol(value);
        return (intValue >= 30000 && intValue <= 600000);
      } else if (strcmp(key, "largeKegMod") == 0) {
        double doubleValue = atof(value);
        return (doubleValue >= 1.0 && doubleValue <= 3.0);
      }
      // Add other parameter validations
      return true;
    }
    ```

- [ ] **Redundant storage**
  - Store critical configuration in EEPROM as backup
  - Implement priority system (SD first, then EEPROM)
  - Example:
    ```cpp
    void saveConfigToEEPROM() {
      int addr = 0;
      
      // Store version marker
      EEPROM.put(addr, (byte)0xAA);
      addr += sizeof(byte);
      
      // Store critical values
      EEPROM.put(addr, washTimer);
      addr += sizeof(washTimer);
      EEPROM.put(addr, saniTimer);
      addr += sizeof(saniTimer);
      EEPROM.put(addr, largeKegMod);
      addr += sizeof(largeKegMod);
      
      // Store checksum
      byte checksum = calculateChecksum();
      EEPROM.put(addr, checksum);
    }
    ```

- [ ] **Configuration file checksums**
  - Implement CRC or simple checksum for config file
  - Verify checksums when loading configuration
  - Example:
    ```cpp
    byte calculateChecksum(const char* data, size_t length) {
      byte checksum = 0;
      for (size_t i = 0; i < length; i++) {
        checksum ^= data[i];
      }
      return checksum;
    }
    ```

- [ ] **Individual setting fallbacks**
  - Handle invalid settings individually rather than reverting all settings
  - Log when fallbacks are used
  - Example:
    ```cpp
    void processSetting(const char* key, const char* value) {
      if (strcmp(key, "washTimer") == 0) {
        long intValue = atol(value);
        if (intValue >= 30000 && intValue <= 600000) {
          washTimer = intValue;
        } else {
          diagnostics_logEvent("Invalid washTimer, using default");
          // Keep existing value or use default
        }
      }
      // Process other settings similarly
    }
    ```

## Priority 4: State Machine Robustness (KegStateMachine.cpp)

Ensuring the state machine never gets stuck or enters invalid states.

- [ ] **State timeouts**
  - Add maximum duration for each state
  - Implement timeout handling for all states
  - Example:
    ```cpp
    void checkStateTimeout() {
      static const unsigned long MAX_STATE_DURATION[] = {
        60000,   // STATE_STARTUP - 1 minute
        300000,  // STATE_DRAINING - 5 minutes
        300000,  // STATE_RINSING - 5 minutes
        600000,  // STATE_WASHING - 10 minutes
        600000,  // STATE_SANITIZE - 10 minutes
        300000,  // STATE_PRESSURE - 5 minutes
        3600000, // STATE_FINISHED - 1 hour
        3600000  // STATE_ERROR - 1 hour
      };
      
      if (currentState < sizeof(MAX_STATE_DURATION)/sizeof(MAX_STATE_DURATION[0])) {
        if (timers_getStateElapsed() > MAX_STATE_DURATION[currentState]) {
          // Log the timeout
          diagnostics_logEvent("State timeout");
          // Handle the timeout - could go to error state or next state
          stateMachine_changeState(STATE_ERROR);
        }
      }
    }
    ```

- [ ] **State history tracking**
  - Maintain history of recent state changes
  - Use for diagnostics and recovery
  - Example:
    ```cpp
    #define STATE_HISTORY_SIZE 10
    
    struct StateChange {
      byte fromState;
      byte toState;
      unsigned long timestamp;
    };
    
    StateChange stateHistory[STATE_HISTORY_SIZE];
    int stateHistoryIndex = 0;
    
    void recordStateChange(byte fromState, byte toState) {
      stateHistory[stateHistoryIndex].fromState = fromState;
      stateHistory[stateHistoryIndex].toState = toState;
      stateHistory[stateHistoryIndex].timestamp = millis();
      
      stateHistoryIndex = (stateHistoryIndex + 1) % STATE_HISTORY_SIZE;
    }
    ```

- [ ] **Enhanced state validation**
  - Add preconditions for each state
  - Verify all required resources before state transitions
  - Example:
    ```cpp
    bool canEnterState(byte targetState) {
      switch (targetState) {
        case STATE_WASHING:
          // Check if water and caustic are available
          if (!isWaterOk || hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) {
            return false;
          }
          break;
        case STATE_PRESSURE:
          // Check if CO2 is available
          if (!isCo2Ok) {
            return false;
          }
          break;
        // Add checks for other states
      }
      return true;
    }
    ```

- [ ] **Recovery mechanisms**
  - Add retry logic for failed state transitions
  - Implement self-healing procedures
  - Example:
    ```cpp
    void handleFailedStateTransition(byte targetState) {
      static byte failureCount = 0;
      
      failureCount++;
      
      if (failureCount > 3) {
        // Too many failures, go to error state
        stateMachine_changeState(STATE_ERROR);
        failureCount = 0;
      } else {
        // Log and try to fix the issue
        diagnostics_logEvent("Retry state transition");
        
        // Try to fix specific issues
        switch (targetState) {
          case STATE_WASHING:
            // Maybe retry heating caustic
            break;
          // Add recovery for other states
        }
        
        // Try again after a delay
        delay(5000);
        stateMachine_changeState(targetState);
      }
    }
    ```

## Priority 5: Timing System Improvements (KegTimers.cpp)

Ensuring accurate and reliable timing even with unusual conditions.

- [ ] **Millis overflow handling**
  - Handle the ~49-day overflow of millis()
  - Use safe time difference calculations
  - Example:
    ```cpp
    unsigned long safeElapsedTime(unsigned long startTime) {
      unsigned long currentTime = millis();
      
      // Handle overflow
      if (currentTime < startTime) {
        return (0xFFFFFFFF - startTime) + currentTime + 1;
      }
      
      return currentTime - startTime;
    }
    ```

- [ ] **Timing validation**
  - Check for unreasonable time jumps
  - Detect clock drift or failures
  - Example:
    ```cpp
    void validateTimerUpdate() {
      static unsigned long lastUpdateTime = 0;
      unsigned long currentTime = millis();
      unsigned long elapsedTime = currentTime - lastUpdateTime;
      
      // If time jump is too large, something might be wrong
      if (lastUpdateTime > 0 && elapsedTime > 1000) {
        diagnostics_logEvent("Large timer jump detected");
      }
      
      lastUpdateTime = currentTime;
    }
    ```

- [ ] **Backup timing mechanisms**
  - Implement secondary timing for critical functions
  - Use the RTC if available
  - Example:
    ```cpp
    unsigned long getSecondaryTimestamp() {
      // This is a placeholder - real implementation would
      // use RTC or other secondary timing source
      static unsigned long counter = 0;
      return ++counter;
    }
    
    bool verifyTimingConsistency() {
      static unsigned long lastMillis = 0;
      static unsigned long lastSecondary = 0;
      
      unsigned long currentMillis = millis();
      unsigned long currentSecondary = getSecondaryTimestamp();
      
      // Calculate deltas (how much each has increased)
      unsigned long millisDelta = currentMillis - lastMillis;
      unsigned long secondaryDelta = currentSecondary - lastSecondary;
      
      // Check if the secondary timing source roughly agrees with millis()
      bool consistent = (abs((long)(millisDelta - secondaryDelta)) < 100);
      
      lastMillis = currentMillis;
      lastSecondary = currentSecondary;
      
      return consistent;
    }
    ```

## Priority 6: Diagnostic Improvements (KegDiagnostics.cpp)

Enhanced diagnostics for better troubleshooting and problem prevention.

- [ ] **Comprehensive logging system**
  - Log all state changes, errors, and key events
  - Store logs on SD card with timestamps
  - Example:
    ```cpp
    void enhancedLogging(const char* eventType, const char* message) {
      if (!SD.exists("logs")) {
        SD.mkdir("logs");
      }
      
      File logFile = SD.open("logs/system.log", FILE_WRITE);
      if (logFile) {
        // Write timestamp
        logFile.print(millis());
        logFile.print(",");
        
        // Write event type
        logFile.print(eventType);
        logFile.print(",");
        
        // Write state
        logFile.print(currentState);
        logFile.print(",");
        
        // Write message
        logFile.println(message);
        
        logFile.close();
      }
    }
    ```

- [ ] **Self-test sequences**
  - Enhance diagnostic test mode
  - Add automated self-tests on startup
  - Example:
    ```cpp
    bool runSelfTest() {
      bool allTestsPassed = true;
      display_showMessage("Running self-test...");
      
      // Test sensors
      if (!testSensors()) {
        allTestsPassed = false;
        diagnostics_logEvent("Sensor test failed");
      }
      
      // Test outputs
      if (!testOutputs()) {
        allTestsPassed = false;
        diagnostics_logEvent("Output test failed");
      }
      
      // Test SD card
      if (!testSDCard()) {
        allTestsPassed = false;
        diagnostics_logEvent("SD card test failed");
      }
      
      return allTestsPassed;
    }
    ```

- [ ] **Performance monitoring**
  - Track loop execution time
  - Monitor memory usage
  - Example:
    ```cpp
    void monitorPerformance() {
      static unsigned long lastLoopTime = 0;
      static unsigned long maxLoopTime = 0;
      static unsigned long loopCount = 0;
      static unsigned long totalLoopTime = 0;
      
      unsigned long currentTime = millis();
      unsigned long loopTime = currentTime - lastLoopTime;
      lastLoopTime = currentTime;
      
      // Update statistics
      if (loopTime > maxLoopTime) {
        maxLoopTime = loopTime;
      }
      
      totalLoopTime += loopTime;
      loopCount++;
      
      // Once per minute, log performance metrics
      if (loopCount % 6000 == 0) {
        diagnostics_logEvent("Perf: Avg=" + String(totalLoopTime/loopCount) + "ms, Max=" + String(maxLoopTime) + "ms");
        
        // Reset statistics
        maxLoopTime = 0;
        totalLoopTime = 0;
        loopCount = 0;
      }
    }
    ```

- [ ] **Remote diagnostics capability**
  - Add serial communication protocol for external monitoring
  - Implement command interface for diagnostics
  - Example:
    ```cpp
    void processSerialCommands() {
      if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        
        if (command == "STATUS") {
          // Send status information
          Serial.print("STATE:");
          Serial.println(currentState);
          Serial.print("TEMP:");
          Serial.println(hardware_getCausticTemp());
          // Add more status info
        } else if (command == "LOG") {
          // Send recent log entries
          // Implementation depends on logging system
        } else if (command.startsWith("TEST:")) {
          // Run specific test based on command
          String testName = command.substring(5);
          runSpecificTest(testName);
        }
      }
    }
    ```

## Implementation Plan

1. Start with the highest priority improvements (Hardware Interface)
2. Test each improvement individually before moving to the next
3. Document all changes in code comments
4. Update the user manual with new features and behaviors
5. Create a test plan for verifying each improvement

## Project Knowledge Management

Converting code artifacts and discussions into lasting project knowledge is crucial for long-term success. Here's how to transform our work into valuable documentation:

### 1. Technical Documentation Package

- **System Design Document**
  - [ ] Convert state table into a formal design document
  - [ ] Create hardware interface specification with all I/O and pinouts
  - [ ] Document software architecture and component interactions
  - [ ] Create timing diagrams for each state's operations

- **Code Documentation**
  - [ ] Add comprehensive comments to all source files
  - [ ] Document function parameters, return values, and purposes
  - [ ] Set up Doxygen to generate formatted documentation from code

### 2. User-Facing Materials

- **Operator's Manual**
  - [ ] Detail proper startup procedures and sequences
  - [ ] Explain what operators should see during each phase
  - [ ] Create error handling guide with troubleshooting steps
  - [ ] Document regular maintenance requirements

- **Quick Reference Guide**
  - [ ] Create chart showing display state meanings
  - [ ] List common issues with solutions
  - [ ] Document emergency procedures

### 3. Visual Learning Materials

- **Process Flow Diagrams**
  - [ ] Create detailed state flow chart 
  - [ ] Develop valve timing diagrams
  - [ ] Include electrical schematics for all components
  - [ ] Create fluid flow diagrams for each state

- **Instructional Media**
  - [ ] Record training videos of system operation
  - [ ] Create annotated photos of equipment
  - [ ] Consider developing a training simulator

### 4. Knowledge Management System

- **Project Wiki**
  - [ ] Document design decisions and rationales
  - [ ] Link requirements to specific implementations
  - [ ] Track development history and versions
  - [ ] Maintain list of known limitations

- **Technical Knowledge Base**
  - [ ] Build searchable database of issues and solutions
  - [ ] Create FAQ section for operation and maintenance
  - [ ] Develop troubleshooting decision trees

### 5. Quality and Verification Documents

- **Test Procedures**
  - [ ] Create acceptance test procedures
  - [ ] Develop regular testing guide
  - [ ] Document calibration instructions

- **Regulatory Compliance**
  - [ ] Compile safety documentation
  - [ ] Validate sanitization effectiveness
  - [ ] Document electrical safety compliance

### 6. Digital Asset Organization

- **Version Control**
  - [ ] Set up GitHub repository with meaningful commit practices
  - [ ] Create detailed release notes
  - [ ] Tag stable versions for reference

- **Digital Document Management**
  - [ ] Organize documentation in logical hierarchy
  - [ ] Track document revisions and changes
  - [ ] Make all documents text-searchable

### Knowledge Capture Implementation Priorities

1. Begin with the enhanced state table as foundation for documentation
2. Document the caustic heating system (safety-critical component)
3. Create user guide focusing on startup procedure and error recovery
4. Develop troubleshooting guide for common issues
5. Establish documentation standards for ongoing development
