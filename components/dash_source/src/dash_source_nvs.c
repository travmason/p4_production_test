#include "dash_source_store.h"

#include "dash_store.h"

#define KEY  "source_mode"

int dash_source_store_load(int fallback)
{
    return (int)dash_store_get_u32(KEY, (uint32_t)fallback);
}

void dash_source_store_save(int value)
{
    dash_store_set_u32(KEY, (uint32_t)value);
}
