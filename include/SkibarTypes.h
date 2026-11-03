enum GateState {
  GATE_IDLE,
  GATE_GREEN_AUTHORIZED,
  GATE_GREEN_PASSING,
  GATE_RED_ACTIVE,
  GATE_RED_PASSING,
  GATE_REARM_DELAY
};

struct GateConfig {
  long gateId;
  long siteId;
  bool irExists;
  unsigned long relayOnDuration;
  unsigned long rfidAuthTimeout;
  unsigned long irDebounceMs;
  unsigned long invalidCardRedDuration;
  unsigned long redPostPassageDuration;
  unsigned long irRearmDelay;
  int configVersion;
};