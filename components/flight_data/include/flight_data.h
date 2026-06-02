/*
 * Flight data client for the adsb.lol REST API.
 *
 * Queries https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm} and parses the
 * returned aircraft into a caller-supplied array.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLIGHT_CALLSIGN_LEN  12
#define FLIGHT_TYPE_LEN      8

typedef struct {
    char   callsign[FLIGHT_CALLSIGN_LEN];  /* e.g. "DLH9HX" (or hex if no callsign) */
    char   type[FLIGHT_TYPE_LEN];          /* ICAO type, e.g. "A20N" */
    double lat;
    double lon;
    float  track;        /* heading over ground, degrees (0 = north) */
    float  ground_speed; /* knots */
    int    altitude_ft;  /* barometric altitude in feet, -1 if on ground/unknown */
    bool   on_ground;
} aircraft_t;

/**
 * @brief Fetch aircraft within @p radius_nm of (lat, lon).
 *
 * @param lat        Centre latitude (decimal degrees).
 * @param lon        Centre longitude (decimal degrees).
 * @param radius_nm  Search radius in nautical miles (1..250).
 * @param out        Caller array to fill.
 * @param max        Capacity of @p out.
 * @param out_count  Receives the number of aircraft written.
 *
 * @return ESP_OK on success.
 */
esp_err_t flight_data_fetch(double lat, double lon, int radius_nm,
                            aircraft_t *out, size_t max, size_t *out_count);

#ifdef __cplusplus
}
#endif
