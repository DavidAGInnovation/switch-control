#pragma once
#include <stdbool.h>

// A failed listener gets one reconstruction once a local IP is available.
// A second failure in the same network epoch waits for a down/up transition;
// an authenticated request proves recovery and permits a later reconstruction.
typedef struct {
    bool pending;
    bool attempted;
} WifiRecovery;

static inline void wifiRecoveryFault(WifiRecovery *r) {r->pending=true;}
static inline bool wifiRecoveryReady(WifiRecovery *r,bool network_ready) {
    if(!network_ready){r->attempted=false;return false;}
    if(!r->pending||r->attempted)return false;
    r->attempted=true;return true;
}
static inline void wifiRecoveryOpened(WifiRecovery *r) {r->pending=false;}
static inline void wifiRecoveryAuthenticated(WifiRecovery *r) {r->attempted=false;}
