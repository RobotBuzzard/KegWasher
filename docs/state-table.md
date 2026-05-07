# Updated Keg Washer State Table

## State Machine with Enhanced Startup Phase

The keg washer system now features an enhanced state machine with a multi-phase startup process that includes caustic heating and comprehensive system checks.

## Main State Map

| State ID | Name | Description | Active Outputs | Conditions | Duration |
|----------|------|-------------|----------------|------------|----------|
| 0 | `STATE_STARTUP` | System initialization & heating | `causticHeaterOut` (during heating) | See sub-states below | 8-15 minutes (primarily heating time) |
| 1 | `STATE_DRAINING` | Draining keg with air assist | `drainOut` + `airOut` (first 5 seconds) | Requires system ready | 3 min (small) / 4.5 min (large) |
| 2 | `STATE_RINSING` | Rinsing with continuous drain | `waterOut` + `drainOut` (most of cycle), `airOut` + `drainOut` (last 5 seconds) | Requires water | 3 min (small) / 4.5 min (large) |
| 3 | `STATE_WASHING` | Caustic wash cycle | `causticOut`, `pumpOut` | Requires caustic ≥ 50°C | 3 min (small) / 4.5 min (large) |
| 4 | `STATE_SANITIZE` | Sanitizing cycle | `sanitizerOut`, `pumpOut` | Requires sanitizer | 3 min (small) / 4.5 min (large) |
| 5 | `STATE_PRESSURE` | Pressurizing with CO2 | `co2Out` | Requires CO2 | 3 min (small) / 4.5 min (large) |
| 6 | `STATE_FINISHED` | Cycle complete | `alarmOut` | None | Until button press |
| 9 | `STATE_ERROR` | Error condition | `alarmOut` | Based on error code | Until button press |

## Startup Sub-States

| Sub-State ID | Name | Description | Active Outputs | Exit Conditions |
|--------------|------|-------------|----------------|-----------------|
| 0 | `STARTUP_INIT` | Initialize system | None | Automatically advances to HEATING |
| 1 | `STARTUP_HEATING` | Heat caustic solution to optimal temperature | `causticHeaterOut` | Temp reaches 60°C (OPTIMAL_CAUSTIC_TEMP) |
| 2 | `STARTUP_IO_CHECK` | Verify all systems ready | None | All systems check passes |
| 3 | `STARTUP_READY` | Wait for operator to start cycle | None | Cycle start button pressed |

## State Requirements 

### STATE_STARTUP (Enhanced with Heating)
- **Inputs Monitored**: 
  - `causticTemp` (temperature sensor)
  - `causticLevelSensor` (level sensor)
  - `cycleStart` (button)
  - All resource inputs (`airOk`, `co2Ok`, `waterOk`)
- **Required Resources**: 
  - Sufficient caustic level (≥25%)
  - Functional heater
- **Error Conditions**:
  - `ERR_CAUSTIC_LEVEL` (level too low for heating)
  - `ERR_HEATING_TIMEOUT` (heating exceeds 15 minutes)
  - `ERR_HEATING_RATE` (heating too slow, <3°C/min)
  - `ERR_HEATER_OVERTEMP` (temperature exceeds 70°C)
  - Any resource failures

### STATE_DRAINING (With Air Burst)
- **Inputs Monitored**: 
  - `ESTOP` (emergency stop)
  - Timer
  - `airOk` (air pressure)
- **Required Resources**:
  - All systems go
  - Air pressure for initial burst
- **Process Sequence**:
  1. Open drain valve (`drainOut`)
  2. Activate air burst (`airOut`) for first 5 seconds
  3. Continue draining for remainder of cycle
- **Error Conditions**:
  - Emergency stop
  - Air pressure loss during initial burst
  - Resource failure during process
- **Purpose**:
  - The initial air burst forcefully expels old product from the keg
  - Significantly improves draining efficiency
  - Helps remove residue that might not drain by gravity alone

### STATE_RINSING (With Continuous Drain and End Air Burst)
- **Inputs Monitored**: 
  - `waterOk` (water pressure)
  - `ESTOP` (emergency stop)
  - `airOk` (air pressure)
  - Timer
- **Required Resources**:
  - Water
  - Air (for end of cycle)
- **Process Sequence**:
  1. Open water valve (`waterOut`) and drain valve (`drainOut`) simultaneously
  2. Rinse flows through keg and directly out drain for most of the cycle
  3. During final 5 seconds, water turns off and air burst (`airOut`) activates
  4. Air pushes remaining water out of keg
- **Error Conditions**:
  - Water pressure loss
  - Air pressure loss during burst
  - Emergency stop
- **Purpose**:
  - Continuous draining ensures fresh water flows through keg
  - Prevents dilution of later cleaning chemicals with rinse water
  - Final air burst removes residual water
  - Improves efficiency of subsequent wash cycle

### STATE_WASHING (Enhanced Safety)
- **Inputs Monitored**:
  - `causticTemp` (temperature sensor)
  - `ESTOP` (emergency stop)
  - Timer
- **Required Resources**:
  - Caustic solution ≥50°C
- **Error Conditions**:
  - `ERR_CAUSTIC_TEMP` (temperature drops below 50°C)
  - Emergency stop

### STATE_SANITIZE
- **Inputs Monitored**:
  - `ESTOP` (emergency stop)
  - Timer
- **Required Resources**:
  - Pump function
- **Error Conditions**:
  - Emergency stop

### STATE_PRESSURE
- **Inputs Monitored**:
  - `co2Ok` (CO2 pressure)
  - `ESTOP` (emergency stop)
  - Timer  
- **Required Resources**:
  - CO2
- **Error Conditions**:
  - CO2 pressure loss
  - Emergency stop

### STATE_FINISHED
- **Inputs Monitored**:
  - `cycleStart` (button)
  - `ESTOP` (emergency stop)
- **Required Resources**:
  - None
- **Error Conditions**:
  - Emergency stop

### STATE_ERROR
- **Inputs Monitored**:
  - `cycleStart` (button)
- **Required Resources**:
  - None
- **Error Conditions**:
  - N/A (already in error state)

## Error Codes

| Code | Name | Description | Recovery Action |
|------|------|-------------|-----------------|
| 0 | `ERR_NONE` | No error | N/A |
| 1 | `ERR_SD_INIT` | SD card initialization failure | Check SD card |
| 2 | `ERR_CONFIG_FILE` | Configuration file error | Verify config file |
| 3 | `ERR_WATER_PRESSURE` | Water pressure low/unavailable | Check water connection |
| 4 | `ERR_AIR_PRESSURE` | Air pressure low/unavailable | Check air compressor |
| 5 | `ERR_CO2_PRESSURE` | CO2 pressure low/unavailable | Check CO2 tank |
| 6 | `ERR_CAUSTIC_TEMP` | Caustic temperature too low | Wait for reheat |
| 7 | `ERR_ENCLOSURE_TEMP` | Enclosure temperature too high | Check ventilation |
| 8 | `ERR_ESTOP` | Emergency stop activated | Clear E-stop |
| 9 | `ERR_INVALID_STATE` | Invalid state transition | System reset |
| 10 | `ERR_HEATING_TIMEOUT` | Caustic heating taking too long | Check heater |
| 11 | `ERR_HEATING_RATE` | Heating rate too slow | Check heater element |
| 12 | `ERR_CAUSTIC_LEVEL` | Caustic level too low | Refill caustic |
| 13 | `ERR_HEATER_OVERTEMP` | Heater temperature exceeds maximum | Check temp sensor |

## State Flow Diagram with Enhanced Startup

```
                                              +----------------+
                                              |                |
                                              v                |
      +-----------------+               +-----------------+    |
      |                 |               |                 |    |
      |   STATE_ERROR   |<------------->|  STATE_STARTUP  |----+
      |                 |               |                 |
      +-----------------+               +-----------------+
             ^                                  |
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_DRAINING |
             |                           |                 |
             |                           +-----------------+
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_RINSING  |
             |                           |                 |
             |                           +-----------------+
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_WASHING  |
             |                           |                 |
             |                           +-----------------+
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_SANITIZE |
             |                           |                 |
             |                           +-----------------+
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_PRESSURE |
             |                           |                 |
             |                           +-----------------+
             |                                  |
             |                                  v
             |                           +-----------------+
             |                           |                 |
             +---------------------------|  STATE_FINISHED |
                                         |                 |
                                         +-----------------+
```

## Startup Sub-State Flow

```
+-----------------+     +-----------------+     +-----------------+     +-----------------+
|                 |     |                 |     |                 |     |                 |
| STARTUP_INIT    |---->| STARTUP_HEATING |---->| STARTUP_IO_CHECK|---->| STARTUP_READY   |
|                 |     |                 |     |                 |     |                 |
+-----------------+     +-----------------+     +-----------------+     +-----------------+
                              |     ^                    |
                              |     |                    |
                              v     |                    v
                        +-----------------+       +-----------------+
                        |                 |       |                 |
                        |   STATE_ERROR   |<------|   STATE_ERROR   |
                        |                 |       |                 |
                        +-----------------+       +-----------------+
```

## Key Reliability Improvements

1. **Enhanced Startup Process**:
   - Systematic heating with monitoring
   - Comprehensive system validation
   - Clear user feedback during heating

2. **Improved Error Handling**:
   - Expanded error codes (13 distinct errors)
   - More precise error detection
   - Better diagnostic messaging

3. **Resource Monitoring**:
   - Continuous checking of critical resources
   - Prevention of operations without required resources
   - Real-time monitoring during critical phases

4. **Safety Enhancements**:
   - Automatic heater shutdown for multiple conditions
   - Temperature monitoring during washing
   - Emergency stop handling in all states

5. **User Interface**:
   - Progress indicators
   - Estimated time remaining
   - Clear error messages with recovery guidance
