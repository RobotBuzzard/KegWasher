# Keg Washer IO Table

## Digital Inputs

| Name | Pin | Description | Type | Active State | Debounced | Critical | Notes |
|------|-----|-------------|------|-------------|-----------|----------|-------|
| `airOk` | DI6 | Air pressure switch | Digital Input | HIGH | Yes | Yes | Monitors compressed air availability |
| `co2Ok` | DI7 | CO2 pressure switch | Digital Input | HIGH | Yes | Yes | Monitors CO2 gas availability |
| `ESTOP` | DI8 | Emergency stop button | Digital Input | LOW | No | Yes | Immediate hardware interrupt, not debounced to ensure fastest response |
| `waterOk` | A11 | Water pressure switch | Digital Input | HIGH | Yes | Yes | Monitors water supply availability |
| `largeKeg` | IO0 | Keg size selector switch | Digital Input | HIGH | Yes | No | Selects between small (20L) and large (50L) keg modes |
| `cycleStart` | IO1 | Start cycle button | Digital Input | HIGH | Yes | No | Used to initiate washing cycle and acknowledge completion |
| `manualDrain` | IO2 | Manual drain button | Digital Input | HIGH | Yes | No | For manual drain operations and diagnostic mode entry |

## Analog Inputs

| Name | Pin | Description | Type | Bit Depth | Range | Filtering | Redundant | Notes |
|------|-----|-------------|------|-----------|-------|-----------|-----------|-------|
| `enclosureTemp` | A9 | Enclosure temperature sensor | Analog Input | 12-bit | 0-100°C | Low-pass | Yes | Controls cabinet fan for cooling |
| `causticTemp` | A10 | Caustic solution temperature | Analog Input | 12-bit | 0-100°C | Low-pass | Yes | Critical for washing effectiveness, must be >50°C |

## Digital Outputs

| Name | Pin | Description | Type | Active State | Monitored | Watchdog | Notes |
|------|-----|-------------|------|-------------|-----------|----------|-------|
| `co2Out` | IO3 | CO2 solenoid valve | Digital Output | HIGH | Yes | 5s | Controls CO2 flow for pressurizing kegs |
| `alarmOut` | IO4 | Audible/visual alarm | Digital Output | HIGH | No | No | Signals cycle completion or errors |
| `drainOut` | CCIOA0 | Drain solenoid valve | Digital Output | HIGH | Yes | 5s | Controls drain path |
| `waterOut` | CCIOA1 | Water solenoid valve | Digital Output | HIGH | Yes | 5s | Controls water inlet |
| `airOut` | CCIOA2 | Air solenoid valve | Digital Output | HIGH | Yes | 5s | Controls compressed air flow |
| `causticOut` | CCIOA3 | Caustic solution valve | Digital Output | HIGH | Yes | 5s | Controls caustic cleaning solution |
| `pumpOut` | CCIOA4 | Circulation pump | Digital Output | HIGH | Yes | 5s | Main pump for fluid circulation |
| `sanitizerOut` | CCIOA5 | Sanitizer solution valve | Digital Output | HIGH | Yes | 5s | Controls sanitizer solution |

## PWM Outputs

| Name | Pin | Description | Type | Bit Depth | Range | Notes |
|------|-----|-------------|------|-----------|-------|-------|
| `cabinFanPWM` | IO5 | Cabinet cooling fan | PWM Output | 8-bit | 0-255 | Temperature-controlled cooling fan. IO4/IO5 both support PWM via SAME53 TCC; choice is wiring-driven. |

## Communication Ports

| Name | Connector | Description | Type | Notes |
|------|-----------|-------------|------|-------|
| `CcioPort` | COM0 | CCIO-8 expansion module | Serial | Provides additional I/O pins |
| `displayPort` | COM1 | LCD display module | Serial | 128x128 pixel monochrome display |

## Legend

- **Critical**: Failure of this I/O affects system safety or can cause hardware damage
- **Debounced**: Input has software debouncing to prevent false triggers
- **Monitored**: Output has feedback monitoring or watchdog timer
- **Redundant**: Sensor has backup or redundant measurement capability
- **Watchdog**: Timeout period for monitored outputs (seconds)

## State Mapping

The following table shows which outputs are active in each state:

| State | Description | Active Outputs |
|-------|-------------|---------------|
| `STATE_STARTUP` | Initializing | None |
| `STATE_DRAINING` | Draining keg | `drainOut` |
| `STATE_RINSING` | Rinsing with water | `waterOut` |
| `STATE_WASHING` | Caustic wash cycle | `causticOut`, `pumpOut` |
| `STATE_SANITIZE` | Sanitizing cycle | `sanitizerOut`, `pumpOut` |
| `STATE_PRESSURE` | Pressurizing with CO2 | `co2Out` |
| `STATE_FINISHED` | Cycle complete | `alarmOut` |
| `STATE_ERROR` | Error condition | `alarmOut` |

## Error Codes

| Code | Name | Description | Related I/O |
|------|------|-------------|------------|
| 0 | `ERR_NONE` | No error | N/A |
| 1 | `ERR_SD_INIT` | SD card initialization failure | SD card interface |
| 2 | `ERR_CONFIG_FILE` | Configuration file error | SD card interface |
| 3 | `ERR_WATER_PRESSURE` | Water pressure low/unavailable | `waterOk` |
| 4 | `ERR_AIR_PRESSURE` | Air pressure low/unavailable | `airOk` |
| 5 | `ERR_CO2_PRESSURE` | CO2 pressure low/unavailable | `co2Ok` |
| 6 | `ERR_CAUSTIC_TEMP` | Caustic temperature too low | `causticTemp` |
| 7 | `ERR_ENCLOSURE_TEMP` | Enclosure temperature too high | `enclosureTemp` |
| 8 | `ERR_ESTOP` | Emergency stop activated | `ESTOP` |
| 9 | `ERR_INVALID_STATE` | Invalid state transition | N/A |

## Wiring Notes

- All digital inputs use internal pull-up/pull-down resistors
- Solenoid valves include flyback diodes for inductive spike protection
- Temperature sensors use voltage dividers with 10kΩ precision resistors
- Emergency stop wired as normally-closed circuit for fail-safe operation
- All low-voltage control signals optically isolated from main power
