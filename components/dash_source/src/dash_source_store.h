/* Where the chosen mode is kept between power cycles. The firmware keeps it in
   NVS (dash_source_nvs.c); the Mac simulator supplies its own. */
#pragma once

/** Returns the saved value, or @p fallback if there is none. */
int dash_source_store_load(int fallback);

void dash_source_store_save(int value);
