/*
 * LVGL radar scope rendered in the classic amber-on-black ATC style:
 * concentric range rings, and each aircraft drawn as a dot with a velocity
 * leader line and a 4-line data block (callsign / type / flight level / speed).
 *
 * All functions take the LVGL port lock internally, so they are safe to call
 * from any task.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "flight_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which scope elements to draw. Set a field to false to omit that
 *        element entirely.
 */
typedef struct {
    bool clock;     /**< Background analog clock (hands + hour pips). */
    bool rings;     /**< Concentric range rings. */
    bool battery;   /**< Battery indicator. */
    bool header;    /**< Top header line (location / range / count). */
    bool status;    /**< Bottom status line (LINK UP / API ERR / ...). */
    bool labels;    /**< Per-aircraft 4-line data blocks. */
    bool leaders;   /**< Per-aircraft velocity leader lines. */
} radar_ui_opts_t;

/**
 * @brief Build the radar scope on the active LVGL screen.
 *
 * @param zip       Postal-code label shown in the header.
 * @param home_lat  Latitude of the radar centre.
 * @param home_lon  Longitude of the radar centre.
 * @param range_nm  Range of the outer ring, in nautical miles.
 * @param opts      Which elements to draw, or NULL to draw everything.
 */
void radar_ui_create(const char *zip, double home_lat, double home_lon, int range_nm,
                     const radar_ui_opts_t *opts);

/**
 * @brief Replace the plotted aircraft with @p list.
 */
void radar_ui_update(const aircraft_t *list, size_t count);

/**
 * @brief Update the short status string in the header (e.g. "LINK UP").
 */
void radar_ui_set_status(const char *status);

/**
 * @brief Update the battery indicator.
 *
 * @param percent  Charge estimate 0..100.
 * @param present  If false, the indicator is hidden (no battery connected).
 */
void radar_ui_set_battery(int percent, bool present);

#ifdef __cplusplus
}
#endif
